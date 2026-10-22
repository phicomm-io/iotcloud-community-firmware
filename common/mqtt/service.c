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
bool service_mqtt_json_begin(service_mqtt_json_scope *scope,mico_thread_t *owner) {
    scope->token=scope->claim=0;
    if(!owner || !mico_rtos_is_current_thread(owner))return false;
    scope->token=service_json_claim();
    if(!scope->token)return false;
    scope->claim=service_memory_claim(0);
    if(!scope->claim) { service_json_release(scope->token);scope->token=0;return false; }
    if(service_json_scope_begin(scope->token,owner))return true;
    service_json_release(scope->token);service_memory_release(scope->claim);
    scope->token=scope->claim=0;return false;
}
static unsigned service_mqtt_json_finish(service_mqtt_json_scope *scope) {
    unsigned failure=0;
    if(scope->token) {
        failure=service_json_scope_end(scope->token);
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
    if(!wire || failure) {
        if(wire)printbuf_free(wire);
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

/* Owner-only and wrap-safe; an allocation refusal must not rebuild at the
 * 10 ms owner cadence. Commands still run before this optional work. */
static bool snapshot_deferred;
static uint32_t snapshot_retry_at;
static int snapshot_resource_wait(void) {
    snapshot_deferred=true;snapshot_retry_at=mico_rtos_get_time();
    return PH_MQTT_MORE;
}
/* Commands are invoked by the shared owner only after Paho has unwound.
 * Exactly one result wire exists; no response graph/linked backlog crosses a
 * transport callback. The caller holds its receipt credit through this send. */
static bool service_mqtt_capabilities_pending;
void service_mqtt_clear(void) {
    service_mqtt_capabilities_pending = false;
    snapshot_deferred=false;
    ph_mqtt_pending_clear();
}

int service_mqtt_flush(int(*sender)(const char *, const char *, bool)) {
    (void) sender;
    return ph_mqtt_poll(NULL);
}

static void service_mqtt_receipt_release(void *wire) {
    printbuf_free(wire);
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
    bool scoped=claim && service_json_scope_begin(token,ph_mqtt_owner_thread());
    json_object *q = scoped ? service_parse(payload, length) : NULL;
    /* Everything borrowed is now copied (including the fixed failure wire).
     * Do not use payload/length/MessageData again after consuming the entry. */
    bool admitted = ph_mqtt_input_consumed();
    wire = scoped && admitted && !service_json_scope_failed(token) ? service_mqtt_command_wire(q) : NULL;
    if (q)
        json_object_put(q);
    unsigned failure=scoped?service_json_scope_end(token):0;
    if(failure && wire){printbuf_free(wire);wire=NULL;}
    service_memory_release(claim);
    service_json_release(token);
    (void)sender;
    /* The owner takes the independent wire, or copies only the fixed fallback.
     * JSON leases have ended. The captured target survives rename and retry. */
    return ph_mqtt_receipt_send(&target, wire ? wire->buf : busy, wire,
                                service_mqtt_receipt_release);
}
/* A busy graph builder must not permanently skip initial capabilities. The
 * owner retries on its next snapshot; no lock/lease crosses the sender. */
__attribute__((noinline)) static int service_mqtt_try_capabilities(int(*sender)(const char *, const char *, bool), const mqtt_account_t *scope) {
    unsigned token = service_json_claim();
    if (!token)
        return snapshot_resource_wait();
    unsigned claim = service_memory_claim(0);
    if (!claim) {
        service_json_release(token);
        return snapshot_resource_wait();
    }
    if(!service_json_scope_begin(token,ph_mqtt_owner_thread())) {
        service_json_release(token);service_memory_release(claim);return snapshot_resource_wait();
    }
    struct printbuf *wire = service_mqtt_wire(service_product_capabilities());
    unsigned failure=service_json_scope_end(token);
    service_json_release(token);
    service_memory_release(claim);
    if (!wire || failure) {
        if(wire)printbuf_free(wire);
        return snapshot_resource_wait();
    }
    char topic[MQTT_TOPIC_MAX];
    mqtt_leaf(scope, "capabilities", topic, sizeof(topic));
    int rc = sender(topic, wire->buf, true);
    if (rc == MQTT_SUCCESS) {
        service_mqtt_capabilities_pending = false;
        snapshot_deferred=false;
    }
    printbuf_free(wire);
    return rc;
}

__attribute__((noinline)) int service_mqtt_snapshot(int(*sender)(const char *, const char *, bool), const mqtt_account_t *scope) {
    int ready = ph_mqtt_resource_ready();
    if (ready != MQTT_SUCCESS) return ready == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED ? PH_MQTT_MORE : ready;
    if(snapshot_deferred && (uint32_t)(mico_rtos_get_time()-snapshot_retry_at)<1000u)
        return PH_MQTT_MORE;
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
        return snapshot_resource_wait();
    unsigned claim = service_memory_claim(0);
    if (!claim) {
        service_json_release(json_claim);
        return snapshot_resource_wait();
    }
    if(!service_json_scope_begin(json_claim,ph_mqtt_owner_thread())) {
        service_memory_release(claim);service_json_release(json_claim);return snapshot_resource_wait();
    }
    /* OTA job status has no model and was never eligible for retained state. */
    struct printbuf *wire=ota_job_active()?NULL:service_product_status_wire(false,false,NULL);
    unsigned failure=service_json_scope_end(json_claim);
    service_json_release(json_claim);service_memory_release(claim);
    if(!wire || failure) {
        if(wire)printbuf_free(wire);
        return snapshot_resource_wait();
    }
    snapshot_deferred=false;
    char topic[MQTT_TOPIC_MAX];mqtt_leaf(scope,"state",topic,sizeof(topic));
    rc=sender(topic,wire->buf,true);
    printbuf_free(wire);return rc;
}

int service_mqtt_capabilities(int(*sender)(const char *, const char *, bool), const mqtt_account_t *scope) {
    service_mqtt_capabilities_pending = true;
    return service_mqtt_try_capabilities(sender, scope);
}
