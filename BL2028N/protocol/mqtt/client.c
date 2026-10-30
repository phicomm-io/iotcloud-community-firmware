#include <limits.h>
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../src/os.h"
#include "client.h"
#include "queue.h"
#include "../busy_reply.h"
#include "wire.h"
#include "../memory_budget.h"
#include <stdlib.h>
#include <string.h>
uint32_t ph_mqtt_startup_prepare(ph_mqtt_startup *s) {
    uint32_t generation = (ph_os_load(&s->state) >> 2) + 1;
    if (generation >(UINT32_MAX >> 2))
        generation = 1;
    ph_os_store(&s->state, generation << 2);
    return generation;
}

void ph_mqtt_startup_publish(ph_mqtt_startup *s, uint32_t generation, bool success) {
    uint32_t expected = generation << 2;
    /* A stale creator cannot publish into a newer start, even if preempted
     * between checking the generation and publishing the output handle. */
    ph_os_cas32(&s->state, &expected, expected |(success ? 1u : 2u));
}

bool ph_mqtt_startup_wait(ph_mqtt_startup *s, uint32_t generation) {
    uint32_t start = ph_os_now();
    do {
        uint32_t state = ph_os_load(&s->state);
        if ((state >> 2) != generation || (state &3u) == 2)
            return false;
        if ((state &3u) == 1)
            return true;
        ph_os_sleep(1);
    } while ((uint32_t)(ph_os_now() - start) < 1000);
    return false;
}

typedef struct {
    const char *filter;
    messageHandler handler;
}

subscription;
typedef struct {
    mqtt_target_t target;
    char id[65];
    ph_wire_format format;
    bool used;
}

emergency;
typedef struct {
    ph_mqtt_core core;
    ph_os_mutex mutex;
    ph_os_thread owner;
    const mqtt_account_t *account;
    Client *client;
    Network *network;
    int(*read)(Network *, unsigned char *, int, int);
    int(*write)(Network *, unsigned char *, int, int);
    ph_mqtt_io_evidence read_evidence, write_evidence;
    unsigned cause, packet_bytes;
    bool(*accept)(MessageData *, messageHandler);
    int(*outgoing)(const ph_mqtt_item *);
    subscription subscriptions[MAX_MESSAGE_HANDLERS];
    emergency *busy; /* TMCP emergency receipts exist only on the primary. */
    ph_mqtt_rx rx;
    size_t payload_max;
    uint32_t delivered;
    bool initialized, mutex_ready, bound, failed;
    bool turn_active;
    unsigned turn_left;
    uint32_t resource_since, resource_retry_at;
    unsigned resource_packet;
    bool resource_active, resource_seen, resource_owned, native_waiting;
    char native_topic[MQTT_TOPIC_MAX];
    unsigned busy_sending;
    struct {
        mqtt_target_t target;
        const char *wire;
        size_t length;
        void *owned;
        void(*release)(void *);
        char busy[SERVICE_BUSY_BYTES];
    } receipt;
}
port_state;
static size_t ph_bounded_length(const char *s,size_t limit) { size_t n=0;while(n<limit&&s[n])++n;return n; }
static emergency primary_busy[2];
static port_state primary_port;
#ifdef PHICOMM_BEMFA
static port_state *secondary_port;
static ph_os_thread *secondary_thread;
#endif
/* Callback selection is based on the immutable owner binding, never a mutable
 * current-connection global. External producer APIs explicitly use primary. */
static port_state *callback_port(void) {
#ifdef PHICOMM_BEMFA
    ph_os_thread *owner=ph_os_load(&secondary_thread);
    if(owner && ph_os_current(owner))
        return ph_os_load(&secondary_port);
#endif
    return &primary_port;
}
static bool initialized(port_state *ctx);
static __attribute__((noinline)) bool ctx_ph_mqtt_owner(port_state *ctx);
static ph_os_thread *ctx_ph_mqtt_owner_thread(port_state *ctx);
static __attribute__((noinline)) void receipt_clear(port_state *ctx);
static __attribute__((noinline)) void pending_release(port_state *ctx);
static __attribute__((noinline)) bool ctx_ph_mqtt_receipt_pending(port_state *ctx);
static __attribute__((noinline)) void ctx_ph_mqtt_owner_end(port_state *ctx);
static const ph_mqtt_core *ctx_ph_mqtt_debug_core(port_state *ctx);
static __attribute__((noinline)) int ctx_ph_mqtt_app_error(port_state *ctx, unsigned stage, int rc);
static __attribute__((noinline)) void ctx_ph_mqtt_error(port_state *ctx, unsigned stage, int rc);
static __attribute__((noinline)) void ctx_ph_mqtt_session_begin(port_state *ctx);
static __attribute__((noinline)) void ctx_ph_mqtt_lifecycle_event(port_state *ctx, unsigned stage, int rc);
static __attribute__((noinline)) void ctx_ph_mqtt_reconnect(port_state *ctx);
static __attribute__((noinline)) void ctx_ph_mqtt_snapshot(port_state *ctx, ph_mqtt_diagnostics *out);
static __attribute__((noinline)) void ctx_ph_mqtt_outgoing(port_state *ctx, int(*handler)(const ph_mqtt_item *));
static __attribute__((noinline)) void ctx_ph_mqtt_pending_clear(port_state *ctx);
static __attribute__((noinline)) bool ctx_ph_mqtt_owner_init(port_state *ctx, const mqtt_account_t *account, size_t maximum, ph_os_thread *owner, bool(*accept)(MessageData *, messageHandler));
static bool modern(port_state *ctx, const char *topic, size_t n);
static bool begin(port_state *ctx, Client *c, unsigned stage);
static int end(port_state *ctx, unsigned stage, int rc);
static __attribute__((noinline)) int ctx_ph_mqtt_client_init(port_state *ctx, Client *c, Network *n, unsigned timeout);
static __attribute__((noinline)) int ctx_ph_mqtt_client_deinit(port_state *ctx, Client *c);
static __attribute__((noinline)) int ctx_ph_mqtt_connect(port_state *ctx, Client *c, MQTTPacket_connectData *data);
static __attribute__((noinline)) int ctx_ph_mqtt_disconnect(port_state *ctx, Client *c);
static __attribute__((noinline)) int ctx_ph_mqtt_subscribe(port_state *ctx, Client *c, const char *filter, enum QoS qos, messageHandler handler);
static __attribute__((noinline)) int ctx_ph_mqtt_yield(port_state *ctx, Client *c, int timeout);
static __attribute__((noinline)) int ctx_ph_mqtt_resource_ready(port_state *ctx);
static __attribute__((noinline)) void ctx_ph_mqtt_resource_round_end(port_state *ctx, bool complete);
static __attribute__((noinline)) int ctx_ph_mqtt_publish(port_state *ctx, Client *c, const char *topic, MQTTMessage *m);
static __attribute__((noinline)) int publish_owned(port_state *ctx, Client *c, const char *topic, MQTTMessage *m);
static __attribute__((noinline)) int receipt_retry(port_state *ctx, Client *c);
static __attribute__((noinline)) int ctx_ph_mqtt_receipt_send_bytes(port_state *ctx, const mqtt_target_t *target, const char *wire, size_t length, void *owned, void(*release_wire)(void *));
static __attribute__((noinline)) int ctx_ph_mqtt_receipt_send(port_state *ctx,const mqtt_target_t *target,const char *wire,void *owned,void(*release_wire)(void *))
{ return ctx_ph_mqtt_receipt_send_bytes(ctx,target,wire,wire?strlen(wire):0,owned,release_wire); }

static __attribute__((noinline)) void ctx_ph_mqtt_turn_begin(port_state *ctx, unsigned limit);
static __attribute__((noinline)) void ctx_ph_mqtt_turn_end(port_state *ctx);
static __attribute__((noinline)) uint32_t ctx_ph_mqtt_send_generation(port_state *ctx);
static __attribute__((noinline)) int ctx_ph_mqtt_enqueue_send_generation(port_state *ctx, const char *topic, const void *payload, size_t n, unsigned qos, bool retain, uint32_t generation);
static __attribute__((noinline)) int ctx_ph_mqtt_enqueue_send(port_state *ctx, const char *topic, const void *payload, size_t n, unsigned qos, bool retain);
static int send_emergency(port_state *ctx, Client *c);
static __attribute__((noinline)) bool ctx_ph_mqtt_input_consumed(port_state *ctx);
static __attribute__((noinline)) bool ctx_ph_mqtt_work_pending(port_state *ctx);
static __attribute__((noinline)) int ctx_ph_mqtt_poll(port_state *ctx, Client *c);
static void mutex_lock(void *p) {
    port_state *ctx=p;
    ph_os_lock(&ctx->mutex);
}

static void mutex_unlock(void *p) {
    port_state *ctx=p;
    ph_os_unlock(&ctx->mutex);
}

static bool initialized(port_state *ctx) {
    if(!ctx)return false;
    return ph_os_load(&ctx->initialized);
}

static bool current(void *p) {
    port_state *ctx=p;
    /* core invokes this hook before acquiring its queue mutex. Keep both the
     * handle and its binding under that same stable mutex during rebind. */
    mutex_lock(ctx);
    bool owner = ctx->bound && ph_os_current(&ctx->owner);
    mutex_unlock(ctx);
    return owner;
}

static uint32_t clock_ms(void *p) {
    (void) p;
    return ph_os_now();
}

static bool permit(void *p, size_t bytes) {
    (void) p;
    size_t available=ph_os_free_heap();
    return available > SERVICE_MEMORY_RESERVE + 32u &&
        bytes <= available - SERVICE_MEMORY_RESERVE - 32u;
}

/* Paho replaces buffers with malloc/copy/free. The old allocation is still
 * live during this check, so admit the entire new block, never just a delta.
 * Deferred command dispatch may legitimately publish its completed receipt. */
int mqtt_buffer_allocation_permit(Client *c, size_t bytes) {
    port_state *ctx=callback_port();
    return ctx_ph_mqtt_owner(ctx) && c == ctx->client && ctx->core.protocol_depth &&
        permit(NULL, bytes);
}

/* The SDK calls this only for required storage, never an optional shrink.
 * Record no payload and preserve the first actual failure in this operation. */
void mqtt_buffer_allocation_failed(Client *c, int admitted) {
    port_state *ctx=callback_port();
    if (ctx_ph_mqtt_owner(ctx) && c == ctx->client && ctx->core.protocol_depth && !ctx->cause)
        ctx->cause = admitted ? PH_MQTT_CAUSE_ALLOCATION : PH_MQTT_CAUSE_HEAP_ADMISSION;
}

static void *allocate(void *p, size_t n) {
    (void) p;
    return malloc(n);
}

static void release(void *p, void *q) {
    (void) p;
    free(q);
}

static __attribute__((noinline)) bool ctx_ph_mqtt_owner(port_state *ctx) {
    return initialized(ctx) && current(ctx);
}

static ph_os_thread *ctx_ph_mqtt_owner_thread(port_state *ctx) {
    return ctx_ph_mqtt_owner(ctx) ? &ctx->owner : NULL;
}

static __attribute__((noinline)) void receipt_clear(port_state *ctx) {
    if (ctx->receipt.owned)
        ctx->receipt.release(ctx->receipt.owned);
    ctx->receipt.wire = NULL;
    ctx->receipt.owned = NULL;
    ctx->receipt.release = NULL;
}

/* Only at quiescent teardown. Core clear deliberately refuses borrowed
 * in-flight entries; a deferred owner now explicitly retires its own entries. */
static __attribute__((noinline)) void pending_release(port_state *ctx) {
    receipt_clear(ctx);
    for (unsigned kind = 0; kind < 2; kind++)
        if (ctx->core.inflight[kind])
            ph_mqtt_item_release(&ctx->core, ctx->core.inflight[kind]);
}

static __attribute__((noinline)) bool ctx_ph_mqtt_receipt_pending(port_state *ctx) {
    return ctx_ph_mqtt_owner(ctx) && ctx->receipt.wire;
}

static __attribute__((noinline)) void ctx_ph_mqtt_owner_end(port_state *ctx) {
    if (!ctx_ph_mqtt_owner(ctx) || ctx->client || ctx->core.protocol_depth || ctx->core.dispatch_depth)
        return;
    ph_mqtt_core_accepting(&ctx->core, false);
    pending_release(ctx);
    ph_mqtt_clear(&ctx->core);
    ctx->resource_active = ctx->native_waiting = false;
    mutex_lock(ctx);
    ctx->bound = false;
    mutex_unlock(ctx);
}

static const ph_mqtt_core *ctx_ph_mqtt_debug_core(port_state *ctx) {
    return &ctx->core;
}

static __attribute__((noinline)) int ctx_ph_mqtt_app_error(port_state *ctx, unsigned stage, int rc) {
    if (rc < 0 && stage >= PH_MQTT_ERROR_APP_DISCOVERY_GRAPH &&
        stage <= PH_MQTT_ERROR_APP_RECEIPT_ADMISSION && ctx_ph_mqtt_owner(ctx))
        ph_mqtt_record_operation(&ctx->core, stage);
    return rc;
}

static __attribute__((noinline)) void ctx_ph_mqtt_error(port_state *ctx, unsigned stage, int rc) {
    if (ctx_ph_mqtt_owner(ctx)) {
        if (stage == PH_MQTT_ERROR_SESSION &&
            ctx->core.operation >= PH_MQTT_ERROR_APP_DISCOVERY_GRAPH &&
            ctx->core.operation <= PH_MQTT_ERROR_APP_RECEIPT_ADMISSION)
            stage = ctx->core.operation;
        ph_mqtt_record_failure(&ctx->core,
            ctx->cause ? ctx->cause : (stage >= PH_MQTT_ERROR_INIT && stage <= PH_MQTT_ERROR_YIELD ? PH_MQTT_CAUSE_SDK : PH_MQTT_CAUSE_UNKNOWN), stage, rc,
            ph_os_free_heap(),
            ctx->packet_bytes, ctx->rx.state, &ctx->read_evidence, &ctx->write_evidence);
    }
}

static __attribute__((noinline)) void ctx_ph_mqtt_session_begin(port_state *ctx) {
    if (ctx_ph_mqtt_owner(ctx)) {
        ctx->cause = ctx->packet_bytes = 0;
        memset(&ctx->read_evidence, 0, sizeof(ctx->read_evidence));
        memset(&ctx->write_evidence, 0, sizeof(ctx->write_evidence));
        ph_mqtt_error_session_begin(&ctx->core);
    }
}

static __attribute__((noinline)) void ctx_ph_mqtt_lifecycle_event(port_state *ctx, unsigned stage, int rc) {
    if (ctx_ph_mqtt_owner(ctx))
        ph_mqtt_record_error(&ctx->core, stage, rc, false);
}

static __attribute__((noinline)) void ctx_ph_mqtt_reconnect(port_state *ctx) {
    if (ctx_ph_mqtt_owner(ctx))
        ph_mqtt_record_reconnect(&ctx->core);
}

static __attribute__((noinline)) void ctx_ph_mqtt_snapshot(port_state *ctx, ph_mqtt_diagnostics *out) {
    if (!initialized(ctx)) {
        memset(out, 0, sizeof(*out));
        return;
    }
    ph_mqtt_core_snapshot(&ctx->core, out);
}

static __attribute__((noinline)) void ctx_ph_mqtt_outgoing(port_state *ctx, int(*handler)(const ph_mqtt_item *)) {
    if (ctx_ph_mqtt_owner(ctx) && !ctx->core.protocol_depth)
        ctx->outgoing = handler;
}

static __attribute__((noinline)) void ctx_ph_mqtt_pending_clear(port_state *ctx) {
    if (!ctx_ph_mqtt_owner(ctx) || ctx->core.protocol_depth || ctx->core.dispatch_depth)
        return;
    ph_mqtt_core_accepting(&ctx->core, false);
    pending_release(ctx);
    if (ph_mqtt_clear(&ctx->core)) {
        if(ctx->busy)memset(ctx->busy, 0, 2*sizeof(*ctx->busy));
        ctx->resource_active = ctx->native_waiting = false;
    }
}

static __attribute__((noinline)) bool ctx_ph_mqtt_owner_init(port_state *ctx, const mqtt_account_t *account, size_t maximum, ph_os_thread *owner, bool(*accept)(MessageData *, messageHandler)) {
    bool published = initialized(ctx);
    if (published) {
        mutex_lock(ctx);
        bool bound = ctx->bound;
        bool owner_matches = bound && ph_os_current(&ctx->owner);
        mutex_unlock(ctx);
        if (bound)
            return owner_matches;
    }
    if (!owner || !ph_os_current(owner) || (ctx==&primary_port && !account) || !maximum || maximum > PH_MQTT_PAYLOAD_BYTES)
        return false;
    if (!published && ph_os_mutex_init(&ctx->mutex) != 0)
        return false;
    ctx->mutex_ready = true;
    if(!published && ctx==&primary_port)ctx->busy=primary_busy;
    mutex_lock(ctx);
    ctx->owner = *owner;
    ctx->bound = true;
    mutex_unlock(ctx);
    ctx->outgoing = NULL;
    ctx->account = account;
    ctx->payload_max = maximum;
    ctx->accept = accept;
    ph_mqtt_hooks h = {ctx, mutex_lock, mutex_unlock, current, clock_ms, permit, allocate, release};
    bool ready = published ? ph_mqtt_core_reset(&ctx->core) : ph_mqtt_core_init(&ctx->core, &h);
    if (!ready) {
        mutex_lock(ctx);
        ctx->bound = false;
        mutex_unlock(ctx);
        return false;
    }
    /* First publication releases the constructed mutex, hooks and core. It is
     * never rewritten during rebind; readers may safely enter stable hooks. */
    if (!published) {
        ctx->core.accepting = false;
        ph_os_store(&ctx->initialized, true);
    }
    ph_mqtt_core_accepting(&ctx->core, false);
    return true;
}

bool ph_mqtt_topic_matches(const char *f, const char *t, size_t n) {
    size_t at = 0;
    for (;;) {
        if (*f == '#' && !f[1])
            return true;
        if (!*f)
            return at == n;
        if (at == n)
            return *f == '/' && f[1] == '#' && !f[2];
        if (*f == '+') {
            while (at < n && t[at] != '/')
                at++;
            f++;
        } else if (*f == t[at]) {
            f++;
            at++;
        } else return false;
    }
}

static bool topic_bytes(MessageData *d, const char * *topic, size_t *length) {
    if (!d || !d->message || !d->topicName)
        return false;
    MQTTString *t = d->topicName;
    if (t->cstring) {
        *topic = t->cstring;
        *length = 0;
        while (*length <= PH_MQTT_TOPIC_BYTES && t->cstring[*length]) ++*length;
    } else {
        if (t->lenstring.len < 0)
            return false;
        *topic = t->lenstring.data;
        *length = t->lenstring.len;
    }
    return *topic && *length && *length <= PH_MQTT_TOPIC_BYTES && !memchr(*topic, 0, *length);
}

static bool modern(port_state *ctx, const char *topic, size_t n) {
    if(!ctx->busy || !ctx->account || !ctx->account->base[0])return false;
    size_t base = strlen(ctx->account->base);
    return n == base + 8 && !memcmp(topic, ctx->account->base, base) && !memcmp(topic + base, "/command", 8);
}

static void ingress(MessageData *d) {
    port_state *ctx=callback_port();
    /* Called by Paho only. No product, JSON, OTA, reconnect or publish here. */
    if (!ctx_ph_mqtt_owner(ctx) || !ctx->core.protocol_depth) {
        ctx->failed = true;
        return;
    }
    if (ctx->delivered == ctx->rx.serial)
        return;
    ctx->delivered = ctx->rx.serial;
    const char *topic;
    size_t n;
    if (!topic_bytes(d, &topic, &n)) {
        ctx->failed = true;
        return;
    }
    unsigned route = 0;
    while (route < MAX_MESSAGE_HANDLERS && (!ctx->subscriptions[route].filter || !ph_mqtt_topic_matches(ctx->subscriptions[route].filter, topic, n)))
        route++;
    if (route == MAX_MESSAGE_HANDLERS)
        return;
    if (ctx->accept && !(ctx->accept)(d, ctx->subscriptions[route].handler))
        return;
    MQTTMessage *m = d->message;
    if (m->payloadlen <= ctx->payload_max && ph_mqtt_enqueue_route(&ctx->core, PH_MQTT_COMMAND, topic, n, m->payload, m->payloadlen, m->qos, m->retained, route))
        return;
    /* Retained controls never execute and need no reply. */
    if (m->retained)
        return;
    if (modern(ctx, topic, n))
        for (unsigned i = 0; i < 2; i++) if (!ctx->busy[i].used) {
        mqtt_target_set(&ctx->busy[i].target, ctx->account);
        service_busy_id(m->payload, m->payloadlen, ctx->busy[i].id);
        ctx->busy[i].format=ph_wire_detect(m->payload,m->payloadlen);
        ctx->busy[i].used = true;
        return;
    }
    /* PUBACK is transport-only. An unadmitted request has not executed: drop
     * it visibly instead of tearing down a healthy socket on application load.
     * Never run business without result credit, or send from this callback. */
    ph_mqtt_record_ingress_drop(&ctx->core, modern(ctx, topic, n));
}

static int guarded_read(Network *n, unsigned char *p, int count, int timeout) {
    port_state *ctx=callback_port();
    if (!ctx_ph_mqtt_owner(ctx) || !ctx->core.protocol_depth || n != ctx->network || ctx->rx.failed)
        return -1;
    ph_mqtt_protocol_buffers(&ctx->core, ctx->client->readbuf_size, ctx->client->buf_size);
    uint32_t at = ph_os_now();
    int rc = (ctx->read)(n, p, count, timeout);
    /* Freeze the first anomalous call in this protocol operation. Idle zero
     * reads are ordinary; their deadline evidence is retained until an error. */
    if (!ctx->cause) {
        ctx->read_evidence = (ph_mqtt_io_evidence){count, rc, timeout > 0 ? (unsigned)timeout : 0,
            ph_os_now() - at};
        if ((rc < 0 || (rc > 0 && rc != count)) && !ctx->cause)
            ctx->cause = PH_MQTT_CAUSE_READ;
    }
    if (rc > 0 && !ph_mqtt_rx_feed(&ctx->rx, p, (size_t) rc)) {
        if (!ctx->cause) ctx->cause = PH_MQTT_CAUSE_RX_FRAMING;
        return -1;
    }
    return rc;
}

static int guarded_write(Network *n, unsigned char *p, int count, int timeout) {
    port_state *ctx=callback_port();
    uint32_t at = ph_os_now();
    int rc = (ctx->write)(n, p, count, timeout);
    if (!ctx->cause) {
        ctx->write_evidence = (ph_mqtt_io_evidence){count, rc, timeout > 0 ? (unsigned)timeout : 0,
            ph_os_now() - at};
        /* Positive partial writes are supported by Paho, not a failure. */
        if (rc <= 0 && !ctx->cause) ctx->cause = PH_MQTT_CAUSE_WRITE;
    }
    return rc;
}

static bool begin(port_state *ctx, Client *c, unsigned stage) {
    if (c != ctx->client || ctx->failed || !ph_mqtt_protocol_begin(&ctx->core))
        return false;
    ctx->cause = ctx->packet_bytes = 0;
    memset(&ctx->read_evidence, 0, sizeof(ctx->read_evidence));
    memset(&ctx->write_evidence, 0, sizeof(ctx->write_evidence));
    ph_mqtt_record_operation(&ctx->core, stage);
    return true;
}

static int end(port_state *ctx, unsigned stage, int rc) {
    ph_mqtt_protocol_buffers(&ctx->core, ctx->client->readbuf_size, ctx->client->buf_size);
    ph_mqtt_protocol_end(&ctx->core);
    ph_mqtt_record_operation(&ctx->core, 0);
    rc = ctx->failed ? MQTT_FAILURE : rc;
    if (rc != MQTT_SUCCESS)
        ctx_ph_mqtt_error(ctx, stage, rc);
    else {
        /* Successful protocol activity cannot become a later session cause. */
        ctx->cause = ctx->packet_bytes = 0;
        memset(&ctx->read_evidence, 0, sizeof(ctx->read_evidence));
        memset(&ctx->write_evidence, 0, sizeof(ctx->write_evidence));
    }
    return rc;
}

static __attribute__((noinline)) int ctx_ph_mqtt_client_init(port_state *ctx, Client *c, Network *n, unsigned timeout) {
    if (!ctx_ph_mqtt_owner(ctx) || ctx->core.protocol_depth || ctx->core.dispatch_depth || ctx->client || !c || !n || !n->mqttread)
        return MQTT_FAILURE;
    if (!ph_mqtt_clear(&ctx->core))
        return MQTT_FAILURE;
    ph_mqtt_core_accepting(&ctx->core, true);
    memset(ctx->subscriptions, 0, sizeof(ctx->subscriptions));
    if(ctx->busy)memset(ctx->busy, 0, 2*sizeof(*ctx->busy));
    ctx->client = c;
    ctx->network = n;
    ctx->failed = false;
    ctx->resource_active = ctx->resource_seen = ctx->native_waiting = false;
    ctx->delivered = UINT32_MAX;
    ph_mqtt_rx_init(&ctx->rx, ctx->payload_max + PH_MQTT_TOPIC_BYTES + 9);
    ctx->read = n->mqttread;
    n->mqttread = guarded_read;
    ctx->write = n->mqttwrite;
    if (ctx->write) n->mqttwrite = guarded_write;
    memset(c, 0, sizeof(*c));
    if (!begin(ctx, c, PH_MQTT_ERROR_INIT))
        return MQTT_FAILURE;
    int rc = end(ctx, PH_MQTT_ERROR_INIT, MQTTClientInit(c, n, timeout));
    if (rc != MQTT_SUCCESS)
        ctx_ph_mqtt_client_deinit(ctx, c);
    return rc;
}

static __attribute__((noinline)) int ctx_ph_mqtt_client_deinit(port_state *ctx, Client *c) {
    if (!ctx_ph_mqtt_owner(ctx) || c != ctx->client || ctx->core.protocol_depth || ctx->core.dispatch_depth)
        return MQTT_FAILURE;
    ph_mqtt_core_accepting(&ctx->core, false);
    pending_release(ctx);
    ph_mqtt_clear(&ctx->core);
    ctx->resource_active = ctx->native_waiting = false;
    if(ctx->busy)memset(ctx->busy, 0, 2*sizeof(*ctx->busy));
    /* Deinit is required even after a framing/admission failure. */
    int rc = MQTTClientDeinit(c);
    ph_mqtt_protocol_buffers(&ctx->core, 0, 0);
    if (ctx->network && ctx->network->mqttread == guarded_read)
        ctx->network->mqttread = ctx->read;
    if (ctx->network && ctx->network->mqttwrite == guarded_write)
        ctx->network->mqttwrite = ctx->write;
    ctx->client = NULL;
    ctx->network = NULL;
    return rc;
}

static __attribute__((noinline)) int ctx_ph_mqtt_connect(port_state *ctx, Client *c, MQTTPacket_connectData *data) {
    if (!begin(ctx, c, PH_MQTT_ERROR_CONNECT))
        return MQTT_FAILURE;
    return end(ctx, PH_MQTT_ERROR_CONNECT, MQTTConnect(c, data));
}

static __attribute__((noinline)) int ctx_ph_mqtt_disconnect(port_state *ctx, Client *c) {
    if (!ctx_ph_mqtt_owner(ctx) || c != ctx->client || !ph_mqtt_protocol_begin(&ctx->core))
        return MQTT_FAILURE;
    ph_mqtt_record_operation(&ctx->core, PH_MQTT_ERROR_SESSION);
    return end(ctx, PH_MQTT_ERROR_SESSION, MQTTDisconnect(c));
}

static __attribute__((noinline)) int ctx_ph_mqtt_subscribe(port_state *ctx, Client *c, const char *filter, enum QoS qos, messageHandler handler) {
    if (!filter || !handler || !begin(ctx, c, PH_MQTT_ERROR_SUBSCRIBE))
        return MQTT_FAILURE;
    unsigned at = 0;
    while (at < MAX_MESSAGE_HANDLERS && ctx->subscriptions[at].filter && strcmp(ctx->subscriptions[at].filter, filter))
        at++;
    int rc = MQTT_FAILURE;
    if (at < MAX_MESSAGE_HANDLERS) {
        bool provisional = !ctx->subscriptions[at].filter;
        if (provisional) {
            ctx->subscriptions[at].filter = filter;
            ctx->subscriptions[at].handler = handler;
        }
        /* MQTT permits retained PUBLISH before SUBACK. Paho installs a new
         * handler after SUBACK; its temporary default copies early input into
         * our bounded queue. Business dispatch is forbidden in protocol_depth. */
        messageHandler previous = c->defaultMessageHandler;
        c->defaultMessageHandler = ingress;
        rc = MQTTSubscribe(c, filter, qos, ingress);
        c->defaultMessageHandler = previous;
        if (rc == MQTT_SUCCESS) {
            ctx->subscriptions[at].filter = filter;
            ctx->subscriptions[at].handler = handler;
        } else if (provisional) {
            ph_mqtt_discard_route(&ctx->core, at);
            memset(&ctx->subscriptions[at], 0, sizeof(ctx->subscriptions[at]));
        }
    }
    return end(ctx, PH_MQTT_ERROR_SUBSCRIBE, rc);
}

static __attribute__((noinline)) int ctx_ph_mqtt_yield(port_state *ctx, Client *c, int timeout) {
    if (!begin(ctx, c, PH_MQTT_ERROR_YIELD))
        return MQTT_FAILURE;
    return end(ctx, PH_MQTT_ERROR_YIELD, MQTTYield(c, timeout));
}

/* A resource episode ends only after a full productive round with no pending
 * deferred receipt/SEND/busy work. A smaller successful packet cannot extend
 * the deadline of a larger blocked packet. All differences are wrap-safe. */
static __attribute__((noinline)) int ctx_ph_mqtt_resource_ready(port_state *ctx) {
    if (!ctx_ph_mqtt_owner(ctx) || ctx->failed)
        return MQTT_FAILURE;
    if (!ctx->resource_active)
        return MQTT_SUCCESS;
    uint32_t elapsed = ph_os_now() - ctx->resource_since;
    if (elapsed >= 30000u) {
        ctx->cause = PH_MQTT_CAUSE_RESOURCE_EXPIRED;
        ctx->packet_bytes = ctx->resource_packet;
        memset(&ctx->read_evidence, 0, sizeof(ctx->read_evidence));
        memset(&ctx->write_evidence, 0, sizeof(ctx->write_evidence));
        ctx_ph_mqtt_error(ctx, PH_MQTT_ERROR_PUBLISH, MQTT_FAILURE);
        ctx->failed = true;
        return MQTT_FAILURE;
    }
    if ((uint32_t)(ph_os_now() - ctx->resource_retry_at) < 1000u) {
        ctx->resource_seen = true;
        return PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED;
    }
    return MQTT_SUCCESS;
}

static __attribute__((noinline)) void ctx_ph_mqtt_resource_round_end(port_state *ctx, bool complete) {
    if (!ctx_ph_mqtt_owner(ctx))
        return;
    if (complete && !ctx->resource_seen && !ctx->native_waiting && !ctx->receipt.wire &&
        !ctx->core.inflight[PH_MQTT_SEND] && (!ctx->busy || (!ctx->busy[0].used && !ctx->busy[1].used)))
        ctx->resource_active = ctx->native_waiting = false;
    ctx->resource_seen = false;
}

static __attribute__((noinline)) int ctx_ph_mqtt_publish(port_state *ctx, Client *c, const char *topic, MQTTMessage *m) {
    if (!m || !topic || !m->payload || m->payloadlen > PH_MQTT_PAYLOAD_BYTES || ph_bounded_length(topic, PH_MQTT_TOPIC_BYTES + 1) > PH_MQTT_TOPIC_BYTES || !ctx_ph_mqtt_owner(ctx))
        return MQTT_FAILURE;
    if (c != ctx->client || ctx->failed || ctx->core.protocol_depth)
        return MQTT_FAILURE;
    int ready = ctx_ph_mqtt_resource_ready(ctx);
    if (ready != MQTT_SUCCESS)
        return ready;
    if (ctx->turn_active) {
        if (!ctx->turn_left)
            return PH_MQTT_MORE;
        --ctx->turn_left;
    }
    size_t packet = strlen(topic) + m->payloadlen + 9;
    /* SDK serializes a second copy, while callback RX can coexist. Its own
     * allocation is still fallible; this check is a soft live-heap admission. */
    if (!permit(NULL, packet + 32)) {
        uint32_t now = ph_os_now();
        if (!ctx->resource_active) {
            ctx->resource_since = now;
            ctx->resource_packet = packet;
            ctx->resource_active = true;
        }
        if (!ctx->resource_owned && !ctx->native_waiting) {
            strcpy(ctx->native_topic, topic);
            ctx->native_waiting = true;
        }
        ctx->resource_retry_at = now;
        ctx->resource_seen = true;
        return PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED;
    }
    if (!begin(ctx, c, PH_MQTT_ERROR_PUBLISH))
        return MQTT_FAILURE;
    ctx->packet_bytes = packet;
    /* The emergency wire/target are now owned by this active SDK call's stack.
     * Preflight refusal never retires the slot; PUBACK callbacks can refill it
     * after this transfer. Never clear that possible replacement on return. */
    if (ctx->busy_sending) ctx->busy[ctx->busy_sending - 1].used = false;
    int rc = end(ctx, PH_MQTT_ERROR_PUBLISH, MQTTPublish(c, topic, m));
    if (rc != MQTT_SUCCESS)
        ctx->failed = true;
    else if (!ctx->resource_owned && ctx->native_waiting && !strcmp(ctx->native_topic, topic))
        ctx->native_waiting = false;
    return rc;
}

static __attribute__((noinline)) int publish_owned(port_state *ctx, Client *c, const char *topic, MQTTMessage *m) {
    ctx->resource_owned = true;
    int rc = ctx_ph_mqtt_publish(ctx, c, topic, m);
    ctx->resource_owned = false;
    return rc;
}

static __attribute__((noinline)) int receipt_retry(port_state *ctx, Client *c) {
    int ready = ctx_ph_mqtt_resource_ready(ctx);
    if (ready != MQTT_SUCCESS) {
        if (ready < 0) receipt_clear(ctx);
        return ready;
    }
    char topic[MQTT_TOPIC_MAX];
    if (!mqtt_target_leaf(&ctx->receipt.target, "result", topic, sizeof(topic)))
        return MQTT_FAILURE;
    MQTTMessage m = {0};
    m.qos = QOS1;
    m.payload = (void *)ctx->receipt.wire;
    m.payloadlen = ctx->receipt.length;
    int rc = publish_owned(ctx, c, topic, &m);
    if (rc != PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
        receipt_clear(ctx);
    return rc;
}

static __attribute__((noinline)) int ctx_ph_mqtt_receipt_send_bytes(port_state *ctx, const mqtt_target_t *target, const char *wire, size_t length, void *owned, void(*release_wire)(void *)) {
    if (!ctx_ph_mqtt_owner(ctx) || !target || !wire || ctx->receipt.wire ||
        (owned && !release_wire) || length>INT_MAX || (!owned && length >= sizeof(ctx->receipt.busy))) {
        if (owned && release_wire) release_wire(owned);
        return MQTT_FAILURE;
    }
    ctx->receipt.length=length;
    ctx->receipt.target = *target;
    ctx->receipt.owned = owned;
    ctx->receipt.release = release_wire;
    if (owned) ctx->receipt.wire = wire;
    else {
        memcpy(ctx->receipt.busy,wire,length);ctx->receipt.busy[length]=0;
        ctx->receipt.wire = ctx->receipt.busy;
    }
    return receipt_retry(ctx, ctx->client);
}

static __attribute__((noinline)) void ctx_ph_mqtt_turn_begin(port_state *ctx, unsigned limit) {
    if (ctx_ph_mqtt_owner(ctx) && !ctx->core.protocol_depth && !ctx->core.dispatch_depth) {
        ctx->turn_active = true;
        ctx->turn_left = limit;
    }
}

static __attribute__((noinline)) void ctx_ph_mqtt_turn_end(port_state *ctx) {
    if (ctx_ph_mqtt_owner(ctx))
        ctx->turn_active = false;
}

static __attribute__((noinline)) uint32_t ctx_ph_mqtt_send_generation(port_state *ctx) {
    if (!initialized(ctx))
        return 0;
    mutex_lock(ctx);
    uint32_t generation = ctx->bound && ctx->core.accepting ? ctx->core.generation : 0;
    mutex_unlock(ctx);
    return generation;
}

static __attribute__((noinline)) int ctx_ph_mqtt_enqueue_send_generation(port_state *ctx, const char *topic, const void *payload, size_t n, unsigned qos, bool retain, uint32_t generation) {
    if (!initialized(ctx) || !topic || !generation)
        return MQTT_FAILURE;
    return ph_mqtt_enqueue_generation(&ctx->core, PH_MQTT_SEND, topic, ph_bounded_length(topic, PH_MQTT_TOPIC_BYTES + 1), payload, n, qos, retain, 0, generation) ? MQTT_SUCCESS : MQTT_FAILURE;
}

static __attribute__((noinline)) int ctx_ph_mqtt_enqueue_send(port_state *ctx, const char *topic, const void *payload, size_t n, unsigned qos, bool retain) {
    if (!initialized(ctx) || !topic)
        return MQTT_FAILURE;
    return ph_mqtt_enqueue(&ctx->core, PH_MQTT_SEND, topic, ph_bounded_length(topic, PH_MQTT_TOPIC_BYTES + 1), payload, n, qos, retain) ? MQTT_SUCCESS : MQTT_FAILURE;
}

static size_t client_busy_wire(const char *id,ph_wire_format format,char out[SERVICE_BUSY_BYTES]) {
    if(format!=PH_WIRE_MSGPACK && format!=PH_WIRE_CBOR)return 0;
    ph_mqtt_wire w={(unsigned char *)out,SERVICE_BUSY_BYTES,0,false};unsigned char h[9];size_t n;
#define HEAD(k,v) do {n=ph_wire_head(format,k,v,h);ph_mw_bytes(&w,h,n);}while(0)
#define TEXT(t) do {const char *str_=(t);HEAD(3,strlen(str_));ph_mw_bytes(&w,str_,strlen(str_));}while(0)
    HEAD(5,*id?3:2);if(*id){TEXT("request_id");TEXT(id);}TEXT("code");TEXT("busy");TEXT("data");HEAD(5,0);
#undef TEXT
#undef HEAD
    return w.failed?0:w.used;
}
static int send_emergency(port_state *ctx, Client *c) {
    if(!ctx->busy)return MQTT_SUCCESS;
    for (unsigned i = 0; i < 2; i++)
        if (ctx->busy[i].used) {
        char topic[MQTT_TOPIC_MAX], wire[SERVICE_BUSY_BYTES];
        if (!mqtt_target_leaf(&ctx->busy[i].target, "result", topic, sizeof(topic)))
            return MQTT_FAILURE;
        size_t length=client_busy_wire(ctx->busy[i].id,ctx->busy[i].format,wire);
        MQTTMessage m = {0};
        m.qos = QOS1;
        m.payload = wire;
        m.payloadlen = length;
        ctx->busy_sending = i + 1;
        int rc = publish_owned(ctx, c, topic, &m);
        ctx->busy_sending = 0;
        if (rc != MQTT_SUCCESS)
            return rc;
    }
    return MQTT_SUCCESS;
}

static __attribute__((noinline)) bool ctx_ph_mqtt_input_consumed(port_state *ctx) {
    if (!ctx_ph_mqtt_owner(ctx) || !ctx->core.dispatch_depth || ctx->core.protocol_depth)
        return false;
    ph_mqtt_item_release(&ctx->core, ctx->core.inflight[PH_MQTT_COMMAND]);
    return ph_mqtt_credit_replace(&ctx->core, PH_MQTT_RECEIPT_BYTES);
}

static __attribute__((noinline)) bool ctx_ph_mqtt_work_pending(port_state *ctx) {
    if (!ctx_ph_mqtt_owner(ctx) || ctx->core.protocol_depth || ctx->core.dispatch_depth || ctx->failed)
        return false;
    mutex_lock(ctx);
    bool pending = ctx->core.head[PH_MQTT_COMMAND] || ctx->core.head[PH_MQTT_SEND];
    mutex_unlock(ctx);
    /* Emergency slots are written/retired only by this owner. */
    return pending || ctx->core.inflight[PH_MQTT_SEND] || ctx->receipt.wire || (ctx->busy && (ctx->busy[0].used || ctx->busy[1].used));
}

static __attribute__((noinline)) int ctx_ph_mqtt_poll(port_state *ctx, Client *c) {
    if (!c)
        c = ctx->client;
    if (!ctx_ph_mqtt_owner(ctx) || c != ctx->client || ctx->core.protocol_depth || ctx->core.dispatch_depth)
        return MQTT_FAILURE;
    if (ctx->failed)
        return MQTT_FAILURE;
    int rc = MQTT_SUCCESS;
    ph_mqtt_item *i;
    if (ctx->receipt.wire) {
        rc = receipt_retry(ctx, c);
        if (rc == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
            goto send_one;
        ph_mqtt_credit_release(&ctx->core);
        if (ctx->core.inflight[PH_MQTT_COMMAND])
            ph_mqtt_item_release(&ctx->core, ctx->core.inflight[PH_MQTT_COMMAND]);
        if (rc != MQTT_SUCCESS)
            return rc;
        /* A completed receipt may acknowledge a rename. Return to the session
         * identity/stop check before dispatching another old-session command. */
        return MQTT_SUCCESS;
    }
    rc = send_emergency(ctx, c);
    if (rc == PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
        goto send_one;
    if (rc != MQTT_SUCCESS)
        return rc;
    /* Fixed budgets prevent callbacks arriving during PUBACK from extending
     * this pass. One business command, then at most one external telemetry. */
    i = ph_mqtt_pop(&ctx->core, PH_MQTT_COMMAND);
    if (i) {
        if (!ph_mqtt_credit_acquire(&ctx->core, SERVICE_BUSY_BYTES)) {
            /* Even the fixed failure receipt must have credit before dispatch. */
            if (modern(ctx, i->data, i->topic_length)) {
                char wire[SERVICE_BUSY_BYTES];
                mqtt_target_t target;
                mqtt_target_set(&target, ctx->account);
                const void *payload=ph_mqtt_item_payload(i);char id[65];service_busy_id(payload,i->length,id);
                size_t length=client_busy_wire(id,ph_wire_detect(payload,i->length),wire);
                rc = ctx_ph_mqtt_receipt_send_bytes(ctx, &target, wire,length, NULL, NULL);
                if (rc != PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
                    ph_mqtt_item_release(&ctx->core, i);
                if (rc < 0)
                    return rc;
                goto send_one;
            }
            /* With no outstanding owner credit, acquire failed only because
             * the shared ledger was full. This legacy/nonmodern input has not
             * run and has no modern busy protocol: count/drop it, not the
             * healthy connection. A preexisting credit remains an invariant
             * failure. Only this owner mutates credit; producers cannot race it. */
            if (!ctx->core.credit) {
                if (!i->retained)
                    ph_mqtt_record_ingress_drop(&ctx->core, false);
                ph_mqtt_item_release(&ctx->core, i);
                goto send_one;
            }
            ph_mqtt_item_release(&ctx->core, i);
            return ctx_ph_mqtt_app_error(ctx, PH_MQTT_ERROR_APP_RECEIPT_ADMISSION, MQTT_FAILURE);
        }
        MQTTString topic = MQTTString_initializer;
        topic.cstring = i->data;
        MQTTMessage m = {0};
        m.payload = (void *) ph_mqtt_item_payload(i);
        m.payloadlen = i->length;
        m.qos = i->qos;
        m.retained = i->retained;
        MessageData d = { &m, &topic};
        if (i->generation == ctx->core.generation && ph_mqtt_dispatch_begin(&ctx->core)) {
            ctx->subscriptions[i->route].handler(&d);
            ph_mqtt_dispatch_end(&ctx->core);
        }
        /* A modern handler may consume its borrowed entry after parsing.
         * Never touch that old MessageData/topic/payload after dispatch. */
        if (ctx->core.inflight[PH_MQTT_COMMAND])
            ph_mqtt_item_release(&ctx->core, ctx->core.inflight[PH_MQTT_COMMAND]);
        if (!ctx->receipt.wire)
            ph_mqtt_credit_release(&ctx->core);
        else
            rc = PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED;
        if (ctx->failed)
            return MQTT_FAILURE;
    }
send_one:
    /* Do not repeat TC1 namespace allocation/encoding during the throttle. */
    {
        int ready = ctx_ph_mqtt_resource_ready(ctx);
        if (ready != MQTT_SUCCESS) return ready;
    }
    /* A refused COMMAND consumes its one attempt, not its external SEND turn. */
    i = ctx->core.inflight[PH_MQTT_SEND];
    if (!i) i = ph_mqtt_pop(&ctx->core, PH_MQTT_SEND);
    if (i) {
        MQTTMessage m = {0};
        m.payload = (void *) ph_mqtt_item_payload(i);
        m.payloadlen = i->length;
        m.qos = i->qos;
        m.retained = i->retained;
        int sent = MQTT_SUCCESS;
        if (i->generation == ctx->core.generation) {
            ctx->resource_owned = true;
            sent = ctx->outgoing ? ctx->outgoing(i) : ctx_ph_mqtt_publish(ctx, c, i->data, &m);
            ctx->resource_owned = false;
        }
        if (sent != PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED)
            ph_mqtt_item_release(&ctx->core, i);
        if (sent != MQTT_SUCCESS) rc = sent;
    }
    return rc;
}

/* Stable public namespaces: producer calls never follow callback selection. */
bool ph_mqtt_owner(void) { return ctx_ph_mqtt_owner(&primary_port); }
#ifdef PHICOMM_BEMFA
bool bm_owner(void) { return ctx_ph_mqtt_owner(secondary_port); }
#endif
ph_os_thread *ph_mqtt_owner_thread(void) { return ctx_ph_mqtt_owner_thread(&primary_port); }
#ifdef PHICOMM_BEMFA
ph_os_thread *bm_owner_thread(void) { return ctx_ph_mqtt_owner_thread(secondary_port); }
#endif
bool ph_mqtt_receipt_pending(void) { return ctx_ph_mqtt_receipt_pending(&primary_port); }
#ifdef PHICOMM_BEMFA
bool bm_receipt_pending(void) { return ctx_ph_mqtt_receipt_pending(secondary_port); }
#endif
void ph_mqtt_owner_end(void) { ctx_ph_mqtt_owner_end(&primary_port); }
#ifdef PHICOMM_BEMFA
void bm_owner_end(void) { ctx_ph_mqtt_owner_end(secondary_port); }
#endif
const ph_mqtt_core *ph_mqtt_debug_core(void) { return ctx_ph_mqtt_debug_core(&primary_port); }
#ifdef PHICOMM_BEMFA
const ph_mqtt_core *bm_debug_core(void) { return ctx_ph_mqtt_debug_core(secondary_port); }
#endif
int ph_mqtt_app_error(unsigned stage, int rc) { return ctx_ph_mqtt_app_error(&primary_port, stage, rc); }
#ifdef PHICOMM_BEMFA
int bm_app_error(unsigned stage, int rc) { return ctx_ph_mqtt_app_error(secondary_port, stage, rc); }
#endif
void ph_mqtt_error(unsigned stage, int rc) { ctx_ph_mqtt_error(&primary_port, stage, rc); }
#ifdef PHICOMM_BEMFA
void bm_error(unsigned stage, int rc) { ctx_ph_mqtt_error(secondary_port, stage, rc); }
#endif
void ph_mqtt_session_begin(void) { ctx_ph_mqtt_session_begin(&primary_port); }
#ifdef PHICOMM_BEMFA
void bm_session_begin(void) { ctx_ph_mqtt_session_begin(secondary_port); }
#endif
void ph_mqtt_lifecycle_event(unsigned stage, int rc) { ctx_ph_mqtt_lifecycle_event(&primary_port, stage, rc); }
#ifdef PHICOMM_BEMFA
void bm_lifecycle_event(unsigned stage, int rc) { ctx_ph_mqtt_lifecycle_event(secondary_port, stage, rc); }
#endif
void ph_mqtt_reconnect(void) { ctx_ph_mqtt_reconnect(&primary_port); }
#ifdef PHICOMM_BEMFA
void bm_reconnect(void) { ctx_ph_mqtt_reconnect(secondary_port); }
#endif
void ph_mqtt_snapshot(ph_mqtt_diagnostics *out) { ctx_ph_mqtt_snapshot(&primary_port, out); }
#ifdef PHICOMM_BEMFA
void bm_snapshot(ph_mqtt_diagnostics *out) { ctx_ph_mqtt_snapshot(secondary_port, out); }
#endif
void ph_mqtt_outgoing(int(*handler)(const ph_mqtt_item *)) { ctx_ph_mqtt_outgoing(&primary_port, handler); }
#ifdef PHICOMM_BEMFA
void bm_outgoing(int(*handler)(const ph_mqtt_item *)) { ctx_ph_mqtt_outgoing(secondary_port, handler); }
#endif
void ph_mqtt_pending_clear(void) { ctx_ph_mqtt_pending_clear(&primary_port); }
#ifdef PHICOMM_BEMFA
void bm_pending_clear(void) { ctx_ph_mqtt_pending_clear(secondary_port); }
#endif
bool ph_mqtt_owner_init(const mqtt_account_t *account, size_t maximum, ph_os_thread *owner, bool(*accept)(MessageData *, messageHandler)) { return ctx_ph_mqtt_owner_init(&primary_port, account, maximum, owner, accept); }
#ifdef PHICOMM_BEMFA
bool bm_owner_init(const mqtt_account_t *account, size_t maximum, ph_os_thread *owner, bool(*accept)(MessageData *, messageHandler)) { return ctx_ph_mqtt_owner_init(secondary_port, account, maximum, owner, accept); }
#endif
int ph_mqtt_client_init(Client *c, Network *n, unsigned timeout) { return ctx_ph_mqtt_client_init(&primary_port, c, n, timeout); }
#ifdef PHICOMM_BEMFA
int bm_client_init(Client *c, Network *n, unsigned timeout) { return ctx_ph_mqtt_client_init(secondary_port, c, n, timeout); }
#endif
int ph_mqtt_client_deinit(Client *c) { return ctx_ph_mqtt_client_deinit(&primary_port, c); }
#ifdef PHICOMM_BEMFA
int bm_client_deinit(Client *c) { return ctx_ph_mqtt_client_deinit(secondary_port, c); }
#endif
int ph_mqtt_connect(Client *c, MQTTPacket_connectData *data) { return ctx_ph_mqtt_connect(&primary_port, c, data); }
#ifdef PHICOMM_BEMFA
int bm_connect(Client *c, MQTTPacket_connectData *data) { return ctx_ph_mqtt_connect(secondary_port, c, data); }
#endif
int ph_mqtt_disconnect(Client *c) { return ctx_ph_mqtt_disconnect(&primary_port, c); }
#ifdef PHICOMM_BEMFA
int bm_disconnect(Client *c) { return ctx_ph_mqtt_disconnect(secondary_port, c); }
#endif
int ph_mqtt_subscribe(Client *c, const char *filter, enum QoS qos, messageHandler handler) { return ctx_ph_mqtt_subscribe(&primary_port, c, filter, qos, handler); }
#ifdef PHICOMM_BEMFA
int bm_subscribe(Client *c, const char *filter, enum QoS qos, messageHandler handler) { return ctx_ph_mqtt_subscribe(secondary_port, c, filter, qos, handler); }
#endif
int ph_mqtt_yield(Client *c, int timeout) { return ctx_ph_mqtt_yield(&primary_port, c, timeout); }
#ifdef PHICOMM_BEMFA
int bm_yield(Client *c, int timeout) { return ctx_ph_mqtt_yield(secondary_port, c, timeout); }
#endif
int ph_mqtt_resource_ready(void) { return ctx_ph_mqtt_resource_ready(&primary_port); }
#ifdef PHICOMM_BEMFA
int bm_resource_ready(void) { return ctx_ph_mqtt_resource_ready(secondary_port); }
#endif
void ph_mqtt_resource_round_end(bool complete) { ctx_ph_mqtt_resource_round_end(&primary_port, complete); }
#ifdef PHICOMM_BEMFA
void bm_resource_round_end(bool complete) { ctx_ph_mqtt_resource_round_end(secondary_port, complete); }
#endif
int ph_mqtt_publish(Client *c, const char *topic, MQTTMessage *m) { return ctx_ph_mqtt_publish(&primary_port, c, topic, m); }
int ph_mqtt_publish_bytes(const char *topic,const void *body,size_t length,bool retain) {
    if(!primary_port.client || !ctx_ph_mqtt_owner(&primary_port))return MQTT_FAILURE;
    MQTTMessage m={0};m.qos=QOS1;m.retained=retain;m.payload=(void *)body;m.payloadlen=length;
    int rc=ctx_ph_mqtt_publish(&primary_port,primary_port.client,topic,&m);
    return rc==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED?PH_MQTT_MORE:rc;
}
#ifdef PHICOMM_BEMFA
int bm_publish(Client *c, const char *topic, MQTTMessage *m) { return ctx_ph_mqtt_publish(secondary_port, c, topic, m); }
#endif
int ph_mqtt_receipt_send(const mqtt_target_t *target, const char *wire, void *owned, void(*release_wire)(void *)) { return ctx_ph_mqtt_receipt_send(&primary_port, target, wire, owned, release_wire); }
#ifdef PHICOMM_BEMFA
int bm_receipt_send(const mqtt_target_t *target, const char *wire, void *owned, void(*release_wire)(void *)) { return ctx_ph_mqtt_receipt_send(secondary_port, target, wire, owned, release_wire); }
#endif
void ph_mqtt_turn_begin(unsigned limit) { ctx_ph_mqtt_turn_begin(&primary_port, limit); }
#ifdef PHICOMM_BEMFA
void bm_turn_begin(unsigned limit) { ctx_ph_mqtt_turn_begin(secondary_port, limit); }
#endif
void ph_mqtt_turn_end(void) { ctx_ph_mqtt_turn_end(&primary_port); }
#ifdef PHICOMM_BEMFA
void bm_turn_end(void) { ctx_ph_mqtt_turn_end(secondary_port); }
#endif
uint32_t ph_mqtt_send_generation(void) { return ctx_ph_mqtt_send_generation(&primary_port); }
#ifdef PHICOMM_BEMFA
uint32_t bm_send_generation(void) { return ctx_ph_mqtt_send_generation(secondary_port); }
#endif
int ph_mqtt_enqueue_send_generation(const char *topic, const void *payload, size_t n, unsigned qos, bool retain, uint32_t generation) { return ctx_ph_mqtt_enqueue_send_generation(&primary_port, topic, payload, n, qos, retain, generation); }
#ifdef PHICOMM_BEMFA
int bm_enqueue_send_generation(const char *topic, const void *payload, size_t n, unsigned qos, bool retain, uint32_t generation) { return ctx_ph_mqtt_enqueue_send_generation(secondary_port, topic, payload, n, qos, retain, generation); }
#endif
int ph_mqtt_enqueue_send(const char *topic, const void *payload, size_t n, unsigned qos, bool retain) { return ctx_ph_mqtt_enqueue_send(&primary_port, topic, payload, n, qos, retain); }
#ifdef PHICOMM_BEMFA
int bm_enqueue_send(const char *topic, const void *payload, size_t n, unsigned qos, bool retain) { return ctx_ph_mqtt_enqueue_send(secondary_port, topic, payload, n, qos, retain); }
#endif
bool ph_mqtt_input_consumed(void) { return ctx_ph_mqtt_input_consumed(&primary_port); }
#ifdef PHICOMM_BEMFA
bool bm_input_consumed(void) { return ctx_ph_mqtt_input_consumed(secondary_port); }
#endif
bool ph_mqtt_work_pending(void) { return ctx_ph_mqtt_work_pending(&primary_port); }
#ifdef PHICOMM_BEMFA
bool bm_work_pending(void) { return ctx_ph_mqtt_work_pending(secondary_port); }
#endif
int ph_mqtt_poll(Client *c) { return ctx_ph_mqtt_poll(&primary_port, c); }
#ifdef PHICOMM_BEMFA
int bm_poll(Client *c) { return ctx_ph_mqtt_poll(secondary_port, c); }
#endif
#ifdef PHICOMM_BEMFA
bool bm_transport_allocate(ph_os_thread *owner) {
    if(!owner || !ph_os_current(owner) || secondary_port || !permit(NULL,sizeof(port_state)))return false;
    port_state *next=calloc(1,sizeof(*next));if(!next)return false;
    ph_os_store(&secondary_thread,owner);
    ph_os_store(&secondary_port,next);return true;
}
void bm_transport_release(void) {
    port_state *ctx=secondary_port;
    if(!ctx || !secondary_thread || !ph_os_current(secondary_thread))return;
    if(initialized(ctx))ctx_ph_mqtt_owner_end(ctx);
    if(ctx->bound || ctx->client)return;
    ph_os_store(&secondary_port,NULL);
    ph_os_store(&secondary_thread,NULL);
    if(ctx->mutex_ready)ph_os_mutex_free(&ctx->mutex);
    free(ctx);
}
#endif

int ph_mqtt_receipt_send_bytes(const mqtt_target_t *target,const void *wire,size_t length,void *owned,void(*release_wire)(void *))
{ return ctx_ph_mqtt_receipt_send_bytes(&primary_port,target,wire,length,owned,release_wire); }
