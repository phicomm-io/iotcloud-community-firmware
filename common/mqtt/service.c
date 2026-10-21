#include "../ota/mode.h"
/* Compiled once, shared by all model adapters. */
#include "service.h"
#include "client.h"
#include "../service.h"
#include "printbuf.h"
#include "../json_owned.h"
#include "../busy_reply.h"
#include "json_heap.h"
#include "mico.h"
/* Protected by the existing JSON allocator mutex. The hook never acquires
 * service/core locks; unrelated HTTP/OTA allocation failures cannot taint an
 * MQTT-owner attempt. The static owner handle outlives every bounded scope. */
static mico_thread_t *json_attempt_owner;
static unsigned json_attempt_failure;
void json_heap_failure(unsigned reason) {
    if (json_attempt_owner && mico_rtos_is_current_thread(json_attempt_owner))
        json_attempt_failure |= reason;
}
bool service_mqtt_json_begin(service_mqtt_json_scope *scope,mico_thread_t *owner) {
    scope->token=scope->claim=0;
    if(!owner || !mico_rtos_is_current_thread(owner))return false;
    scope->token=service_json_claim();
    if(!scope->token)return false;
    scope->claim=service_memory_claim(0);
    if(!scope->claim) { service_json_release(scope->token);scope->token=0;return false; }
    json_heap_lock();
    json_attempt_owner=owner;json_attempt_failure=0;
    json_heap_unlock();
    return true;
}
static unsigned service_mqtt_json_finish(service_mqtt_json_scope *scope) {
    unsigned failure=0;
    if(scope->token) {
        json_heap_lock();
        failure=json_attempt_failure;json_attempt_owner=NULL;json_attempt_failure=0;
        json_heap_unlock();
        service_json_release(scope->token);service_memory_release(scope->claim);
        scope->token=scope->claim=0;
    }
    return failure;
}
void service_mqtt_json_end(service_mqtt_json_scope *scope) { (void)service_mqtt_json_finish(scope); }
int service_mqtt_json_error(service_mqtt_json_scope *scope,unsigned stage) {
    unsigned failure=service_mqtt_json_finish(scope);
    return (failure & 3u) && !(failure & 4u) ? PH_MQTT_MORE : ph_mqtt_app_error(stage,MQTT_FAILURE);
}
int service_mqtt_optional_send(int(*sender)(const char *,const char *,bool),const char *topic,json_object *o,bool retain,service_mqtt_json_scope *scope) {
    enum json_owned_error error;
    struct printbuf *wire=json_owned_to_wire_result(o,&error);
    unsigned failure=service_mqtt_json_finish(scope);
    if(!wire) {
        if(error!=JSON_OWNED_INVALID && (failure & 3u) && !(failure & 4u))return PH_MQTT_MORE;
        return ph_mqtt_app_error(PH_MQTT_ERROR_APP_DISCOVERY_ENCODING,MQTT_FAILURE);
    }
    int rc=sender(topic,wire->buf,retain);
    printbuf_free(wire);
    return rc;
}
static struct printbuf *service_mqtt_wire(json_object *o) {
    /* PUBACK waiting can dispatch callbacks or overlap an HTTP status request.
     * Keep only independent wire bytes while waiting, not the JSON object graph. */
    return json_owned_to_wire(o);
}

int service_mqtt_send(int(*sender)(const char *, const char *, bool), const char *topic, json_object *o, bool retain) {
    struct printbuf *wire = service_mqtt_wire(o);
    if (!wire)
        return ph_mqtt_app_error(PH_MQTT_ERROR_APP_DISCOVERY_ENCODING, MQTT_FAILURE);
    int rc = sender(topic, wire->buf, retain);
    printbuf_free(wire);
    return rc;
}

__attribute__((noinline)) static int service_mqtt_publish(int(*sender)(const char *, const char *, bool), const mqtt_account_t *scope, const char *leaf, json_object *o, bool retain, unsigned claim, unsigned json_claim) {
    /* Telemetry allocation failure must not tear down an otherwise live socket. */
    struct printbuf *wire = service_mqtt_wire(o);
    service_json_release(json_claim);
    service_memory_release(claim);
    if (!wire)
        return PH_MQTT_MORE;
    char topic[MQTT_TOPIC_MAX];
    mqtt_leaf(scope, leaf, topic, sizeof(topic));
    int rc = sender(topic, wire->buf, retain);
    printbuf_free(wire);
    return rc;
}
/* Commands are invoked by the shared owner only after Paho has unwound.
 * Exactly one result wire exists; no response graph/linked backlog crosses a
 * transport callback. The caller holds its receipt credit through this send. */
static bool service_mqtt_capabilities_pending;
void service_mqtt_clear(void) {
    service_mqtt_capabilities_pending = false;
    ph_mqtt_pending_clear();
}

int service_mqtt_flush(int(*sender)(const char *, const char *, bool)) {
    (void) sender;
    return ph_mqtt_poll(NULL);
}

int service_mqtt_input(int(*sender)(const char *, const char *, bool), const mqtt_account_t *scope, const void *payload, size_t length, bool retained) {
    if (!ph_mqtt_owner() || !ph_mqtt_debug_core()->dispatch_depth)
        return MQTT_FAILURE;
    if (retained || (ota_job_active() && length > 1024))
        return MQTT_SUCCESS;
    mqtt_target_t target;
    mqtt_target_set(&target, scope);
    char busy[SERVICE_BUSY_BYTES];
    service_busy_wire(payload, length, busy);
    unsigned token = service_json_claim();
    unsigned claim = token ? service_memory_claim(length) : 0;
    struct printbuf *wire = NULL;
    uint64_t failures = service_memory_failures();
    json_object *q = claim ? service_parse(payload, length) : NULL;
    /* Everything borrowed is now copied (including the fixed failure wire).
     * Do not use payload/length/MessageData again after consuming the entry. */
    bool admitted = ph_mqtt_input_consumed();
    json_object *r = claim && admitted && service_memory_failures() == failures ? service_command(q) : NULL;
    if (q)
        json_object_put(q);
    wire = service_mqtt_wire(r);
    service_memory_release(claim);
    service_json_release(token);
    char topic[MQTT_TOPIC_MAX];
    if (!mqtt_target_leaf(&target, "result", topic, sizeof(topic))) {
        if (wire)
            printbuf_free(wire);
        return MQTT_FAILURE;
    }
    int rc = sender(topic, wire ? wire->buf : busy, false);
    if (wire)
        printbuf_free(wire);
    return rc;
}
/* A busy graph builder must not permanently skip initial capabilities. The
 * owner retries on its next snapshot; no lock/lease crosses the sender. */
__attribute__((noinline)) static int service_mqtt_try_capabilities(int(*sender)(const char *, const char *, bool), const mqtt_account_t *scope) {
    unsigned token = service_json_claim();
    if (!token)
        return MQTT_SUCCESS;
    unsigned claim = service_memory_claim(0);
    if (!claim) {
        service_json_release(token);
        return MQTT_SUCCESS;
    }
    struct printbuf *wire = service_mqtt_wire(service_product_capabilities());
    service_json_release(token);
    service_memory_release(claim);
    if (!wire)
        return MQTT_SUCCESS;
    char topic[MQTT_TOPIC_MAX];
    mqtt_leaf(scope, "capabilities", topic, sizeof(topic));
    int rc = sender(topic, wire->buf, true);
    if (rc == MQTT_SUCCESS)
        service_mqtt_capabilities_pending = false;
    printbuf_free(wire);
    return rc;
}

__attribute__((noinline)) int service_mqtt_snapshot(int(*sender)(const char *, const char *, bool), const mqtt_account_t *scope) {
    int rc = MQTT_SUCCESS;
    /* One command was serviced above. A stream of further commands must not
     * starve this bounded periodic snapshot indefinitely. */
    if (service_mqtt_capabilities_pending) {
        rc = service_mqtt_try_capabilities(sender, scope);
        if (rc != MQTT_SUCCESS)
            return rc;
        if (service_mqtt_capabilities_pending)
            return PH_MQTT_MORE;
    }
    unsigned json_claim = service_json_claim();
    if (!json_claim)
        return PH_MQTT_MORE;
    unsigned claim = service_memory_claim(0);
    if (!claim) {
        service_json_release(json_claim);
        return PH_MQTT_MORE;
    }
    json_object *r = service_query("status.get");
    if (!r) {
        service_memory_release(claim);
        service_json_release(json_claim);
        return PH_MQTT_MORE;
    }
    json_object *code = json_object_object_get(r, "code");
    json_object *data = json_object_object_get(r, "data");
    json_object *model = data && json_object_is_type(data, json_type_object)
        ? json_object_object_get(data, "model") : NULL;
    bool valid = code && json_object_is_type(code, json_type_string) &&
        !strcmp(json_object_get_string(code), "ok") && model &&
        json_object_is_type(model, json_type_string) &&
        json_object_get_string_len(model) > 0;
    if (valid)
        json_object_get(data);
    json_object_put(r);
    if (!valid) {
        service_memory_release(claim);
        service_json_release(json_claim);
        return PH_MQTT_MORE;
    }
    return service_mqtt_publish(sender, scope, "state", data, true, claim, json_claim);
}

int service_mqtt_capabilities(int(*sender)(const char *, const char *, bool), const mqtt_account_t *scope) {
    service_mqtt_capabilities_pending = true;
    return service_mqtt_try_capabilities(sender, scope);
}
