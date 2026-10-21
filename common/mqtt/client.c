/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mico.h"
#include "client.h"
#include "queue.h"
#include "../busy_reply.h"
#include "../memory_budget.h"
#include <stdlib.h>
#include <string.h>
uint32_t ph_mqtt_startup_prepare(ph_mqtt_startup *s) {
    uint32_t generation = (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) >> 2) + 1;
    if (generation >(UINT32_MAX >> 2))
        generation = 1;
    __atomic_store_n(&s->state, generation << 2, __ATOMIC_RELEASE);
    return generation;
}

void ph_mqtt_startup_publish(ph_mqtt_startup *s, uint32_t generation, bool success) {
    uint32_t expected = generation << 2;
    /* A stale creator cannot publish into a newer start, even if preempted
     * between checking the generation and publishing the output handle. */
    __atomic_compare_exchange_n(&s->state, &expected, expected |(success ? 1u : 2u), false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

bool ph_mqtt_startup_wait(ph_mqtt_startup *s, uint32_t generation) {
    uint32_t start = mico_rtos_get_time();
    do {
        uint32_t state = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if ((state >> 2) != generation || (state &3u) == 2)
            return false;
        if ((state &3u) == 1)
            return true;
        mico_thread_msleep(1);
    } while ((uint32_t)(mico_rtos_get_time() - start) < 1000);
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
    bool used;
}

emergency;
static struct {
    ph_mqtt_core core;
    mico_mutex_t mutex;
    mico_thread_t owner;
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
    emergency busy[2];
    ph_mqtt_rx rx;
    size_t payload_max;
    uint32_t delivered;
    bool initialized, bound, failed;
    bool turn_active;
    unsigned turn_left;
}

port;
static void mutex_lock(void *p) {
    (void) p;
    mico_rtos_lock_mutex(&port.mutex);
}

static void mutex_unlock(void *p) {
    (void) p;
    mico_rtos_unlock_mutex(&port.mutex);
}

static bool initialized(void) {
    return __atomic_load_n(&port.initialized, __ATOMIC_ACQUIRE);
}

static bool current(void *p) {
    (void) p;
    /* core invokes this hook before acquiring its queue mutex. Keep both the
     * handle and its binding under that same stable mutex during rebind. */
    mutex_lock(NULL);
    bool owner = port.bound && mico_rtos_is_current_thread(&port.owner);
    mutex_unlock(NULL);
    return owner;
}

static uint32_t clock_ms(void *p) {
    (void) p;
    return mico_rtos_get_time();
}

static bool permit(void *p, size_t bytes) {
    (void) p;
    const micoMemInfo_t *m = MicoGetMemoryInfo();
    return m && m->free_memory > 0 && (size_t) m->free_memory >= SERVICE_MEMORY_RESERVE + 32u && bytes <=(size_t) m->free_memory - SERVICE_MEMORY_RESERVE -32u;
}

static void *allocate(void *p, size_t n) {
    (void) p;
    return malloc(n);
}

static void release(void *p, void *q) {
    (void) p;
    free(q);
}

bool ph_mqtt_owner(void) {
    return initialized() && current(NULL);
}

void ph_mqtt_owner_end(void) {
    if (!ph_mqtt_owner() || port.client || port.core.protocol_depth || port.core.dispatch_depth)
        return;
    ph_mqtt_core_accepting(&port.core, false);
    ph_mqtt_clear(&port.core);
    mutex_lock(NULL);
    port.bound = false;
    mutex_unlock(NULL);
}

const ph_mqtt_core *ph_mqtt_debug_core(void) {
    return &port.core;
}

int ph_mqtt_app_error(unsigned stage, int rc) {
    if (rc < 0 && stage >= PH_MQTT_ERROR_APP_DISCOVERY_GRAPH &&
        stage <= PH_MQTT_ERROR_APP_RECEIPT_ADMISSION && ph_mqtt_owner())
        ph_mqtt_record_operation(&port.core, stage);
    return rc;
}

void ph_mqtt_error(unsigned stage, int rc) {
    if (ph_mqtt_owner()) {
        if (stage == PH_MQTT_ERROR_SESSION &&
            port.core.operation >= PH_MQTT_ERROR_APP_DISCOVERY_GRAPH &&
            port.core.operation <= PH_MQTT_ERROR_APP_RECEIPT_ADMISSION)
            stage = port.core.operation;
        const micoMemInfo_t *m = MicoGetMemoryInfo();
        ph_mqtt_record_failure(&port.core,
            port.cause ? port.cause : (stage >= PH_MQTT_ERROR_INIT && stage <= PH_MQTT_ERROR_YIELD ? PH_MQTT_CAUSE_SDK : PH_MQTT_CAUSE_UNKNOWN), stage, rc,
            m && m->free_memory > 0 ? (uint32_t)m->free_memory : 0,
            port.packet_bytes, port.rx.state, &port.read_evidence, &port.write_evidence);
    }
}

void ph_mqtt_session_begin(void) {
    if (ph_mqtt_owner()) {
        port.cause = port.packet_bytes = 0;
        memset(&port.read_evidence, 0, sizeof(port.read_evidence));
        memset(&port.write_evidence, 0, sizeof(port.write_evidence));
        ph_mqtt_error_session_begin(&port.core);
    }
}

void ph_mqtt_lifecycle_event(unsigned stage, int rc) {
    if (ph_mqtt_owner())
        ph_mqtt_record_error(&port.core, stage, rc, false);
}

void ph_mqtt_reconnect(void) {
    if (ph_mqtt_owner())
        ph_mqtt_record_reconnect(&port.core);
}

void ph_mqtt_snapshot(ph_mqtt_diagnostics *out) {
    if (!initialized()) {
        memset(out, 0, sizeof(*out));
        return;
    }
    ph_mqtt_core_snapshot(&port.core, out);
}

void ph_mqtt_outgoing(int(*handler)(const ph_mqtt_item *)) {
    if (ph_mqtt_owner() && !port.core.protocol_depth)
        port.outgoing = handler;
}

void ph_mqtt_pending_clear(void) {
    if (!ph_mqtt_owner())
        return;
    ph_mqtt_core_accepting(&port.core, false);
    if (ph_mqtt_clear(&port.core))
        memset(port.busy, 0, sizeof(port.busy));
}

bool ph_mqtt_owner_init(const mqtt_account_t *account, size_t maximum, mico_thread_t *owner, bool(*accept)(MessageData *, messageHandler)) {
    bool published = initialized();
    if (published) {
        mutex_lock(NULL);
        bool bound = port.bound;
        bool owner_matches = bound && mico_rtos_is_current_thread(&port.owner);
        mutex_unlock(NULL);
        if (bound)
            return owner_matches;
    }
    if (!owner || !mico_rtos_is_current_thread(owner) || !account || !maximum || maximum > PH_MQTT_PAYLOAD_BYTES)
        return false;
    if (!published && mico_rtos_init_mutex(&port.mutex) != kNoErr)
        return false;
    mutex_lock(NULL);
    port.owner = *owner;
    port.bound = true;
    mutex_unlock(NULL);
    port.outgoing = NULL;
    port.account = account;
    port.payload_max = maximum;
    port.accept = accept;
    ph_mqtt_hooks h = {NULL, mutex_lock, mutex_unlock, current, clock_ms, permit, allocate, release};
    bool ready = published ? ph_mqtt_core_reset(&port.core) : ph_mqtt_core_init(&port.core, &h);
    if (!ready) {
        mutex_lock(NULL);
        port.bound = false;
        mutex_unlock(NULL);
        return false;
    }
    /* First publication releases the constructed mutex, hooks and core. It is
     * never rewritten during rebind; readers may safely enter stable hooks. */
    if (!published) {
        port.core.accepting = false;
        __atomic_store_n(&port.initialized, true, __ATOMIC_RELEASE);
    }
    ph_mqtt_core_accepting(&port.core, false);
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
        *length = strnlen(t->cstring, PH_MQTT_TOPIC_BYTES + 1);
    } else {
        if (t->lenstring.len < 0)
            return false;
        *topic = t->lenstring.data;
        *length = t->lenstring.len;
    }
    return *topic && *length && *length <= PH_MQTT_TOPIC_BYTES && !memchr(*topic, 0, *length);
}

static bool modern(const char *topic, size_t n) {
    size_t base = strlen(port.account->base);
    return n == base + 8 && !memcmp(topic, port.account->base, base) && !memcmp(topic + base, "/command", 8);
}

static void ingress(MessageData *d) {
    /* Called by Paho only. No product, JSON, OTA, reconnect or publish here. */
    if (!ph_mqtt_owner() || !port.core.protocol_depth) {
        port.failed = true;
        return;
    }
    if (port.delivered == port.rx.serial)
        return;
    port.delivered = port.rx.serial;
    const char *topic;
    size_t n;
    if (!topic_bytes(d, &topic, &n)) {
        port.failed = true;
        return;
    }
    unsigned route = 0;
    while (route < MAX_MESSAGE_HANDLERS && (!port.subscriptions[route].filter || !ph_mqtt_topic_matches(port.subscriptions[route].filter, topic, n)))
        route++;
    if (route == MAX_MESSAGE_HANDLERS)
        return;
    if (port.accept && !port.accept(d, port.subscriptions[route].handler))
        return;
    MQTTMessage *m = d->message;
    if (m->payloadlen <= port.payload_max && ph_mqtt_enqueue_route(&port.core, PH_MQTT_COMMAND, topic, n, m->payload, m->payloadlen, m->qos, m->retained, route))
        return;
    /* Retained controls never execute and need no reply. */
    if (m->retained)
        return;
    if (modern(topic, n))
        for (unsigned i = 0; i < 2; i++) if (!port.busy[i].used) {
        mqtt_target_set(&port.busy[i].target, port.account);
        service_busy_id(m->payload, m->payloadlen, port.busy[i].id);
        port.busy[i].used = true;
        return;
    }
    /* PUBACK is transport-only. An unadmitted request has not executed: drop
     * it visibly instead of tearing down a healthy socket on application load.
     * Never run business without result credit, or send from this callback. */
    ph_mqtt_record_ingress_drop(&port.core, modern(topic, n));
}

static int guarded_read(Network *n, unsigned char *p, int count, int timeout) {
    if (!ph_mqtt_owner() || !port.core.protocol_depth || n != port.network || port.rx.failed)
        return -1;
    ph_mqtt_protocol_buffers(&port.core, port.client->readbuf_size, port.client->buf_size);
    uint32_t at = mico_rtos_get_time();
    int rc = port.read(n, p, count, timeout);
    /* Freeze the first anomalous call in this protocol operation. Idle zero
     * reads are ordinary; their deadline evidence is retained until an error. */
    if (!port.cause) {
        port.read_evidence = (ph_mqtt_io_evidence){count, rc, timeout > 0 ? (unsigned)timeout : 0,
            mico_rtos_get_time() - at};
        if ((rc < 0 || (rc > 0 && rc != count)) && !port.cause)
            port.cause = PH_MQTT_CAUSE_READ;
    }
    if (rc > 0 && !ph_mqtt_rx_feed(&port.rx, p, (size_t) rc)) {
        if (!port.cause) port.cause = PH_MQTT_CAUSE_RX_FRAMING;
        return -1;
    }
    return rc;
}

static int guarded_write(Network *n, unsigned char *p, int count, int timeout) {
    uint32_t at = mico_rtos_get_time();
    int rc = port.write(n, p, count, timeout);
    if (!port.cause) {
        port.write_evidence = (ph_mqtt_io_evidence){count, rc, timeout > 0 ? (unsigned)timeout : 0,
            mico_rtos_get_time() - at};
        /* Positive partial writes are supported by Paho, not a failure. */
        if (rc <= 0 && !port.cause) port.cause = PH_MQTT_CAUSE_WRITE;
    }
    return rc;
}

static bool begin(Client *c, unsigned stage) {
    if (c != port.client || port.failed || !ph_mqtt_protocol_begin(&port.core))
        return false;
    port.cause = port.packet_bytes = 0;
    memset(&port.read_evidence, 0, sizeof(port.read_evidence));
    memset(&port.write_evidence, 0, sizeof(port.write_evidence));
    ph_mqtt_record_operation(&port.core, stage);
    return true;
}

static int end(unsigned stage, int rc) {
    ph_mqtt_protocol_buffers(&port.core, port.client->readbuf_size, port.client->buf_size);
    ph_mqtt_protocol_end(&port.core);
    ph_mqtt_record_operation(&port.core, 0);
    rc = port.failed ? MQTT_FAILURE : rc;
    if (rc != MQTT_SUCCESS)
        ph_mqtt_error(stage, rc);
    else {
        /* Successful protocol activity cannot become a later session cause. */
        port.cause = port.packet_bytes = 0;
        memset(&port.read_evidence, 0, sizeof(port.read_evidence));
        memset(&port.write_evidence, 0, sizeof(port.write_evidence));
    }
    return rc;
}

int ph_mqtt_client_init(Client *c, Network *n, unsigned timeout) {
    if (!ph_mqtt_owner() || port.core.protocol_depth || port.core.dispatch_depth || port.client || !c || !n || !n->mqttread)
        return MQTT_FAILURE;
    if (!ph_mqtt_clear(&port.core))
        return MQTT_FAILURE;
    ph_mqtt_core_accepting(&port.core, true);
    memset(port.subscriptions, 0, sizeof(port.subscriptions));
    memset(port.busy, 0, sizeof(port.busy));
    port.client = c;
    port.network = n;
    port.failed = false;
    port.delivered = UINT32_MAX;
    ph_mqtt_rx_init(&port.rx, port.payload_max + PH_MQTT_TOPIC_BYTES + 9);
    port.read = n->mqttread;
    n->mqttread = guarded_read;
    port.write = n->mqttwrite;
    if (port.write) n->mqttwrite = guarded_write;
    memset(c, 0, sizeof(*c));
    if (!begin(c, PH_MQTT_ERROR_INIT))
        return MQTT_FAILURE;
    int rc = end(PH_MQTT_ERROR_INIT, MQTTClientInit(c, n, timeout));
    if (rc != MQTT_SUCCESS)
        ph_mqtt_client_deinit(c);
    return rc;
}

int ph_mqtt_client_deinit(Client *c) {
    if (!ph_mqtt_owner() || c != port.client || port.core.protocol_depth || port.core.dispatch_depth)
        return MQTT_FAILURE;
    ph_mqtt_core_accepting(&port.core, false);
    ph_mqtt_clear(&port.core);
    memset(port.busy, 0, sizeof(port.busy));
    /* Deinit is required even after a framing/admission failure. */
    int rc = MQTTClientDeinit(c);
    ph_mqtt_protocol_buffers(&port.core, 0, 0);
    if (port.network && port.network->mqttread == guarded_read)
        port.network->mqttread = port.read;
    if (port.network && port.network->mqttwrite == guarded_write)
        port.network->mqttwrite = port.write;
    port.client = NULL;
    port.network = NULL;
    return rc;
}

int ph_mqtt_connect(Client *c, MQTTPacket_connectData *data) {
    if (!begin(c, PH_MQTT_ERROR_CONNECT))
        return MQTT_FAILURE;
    return end(PH_MQTT_ERROR_CONNECT, MQTTConnect(c, data));
}

int ph_mqtt_disconnect(Client *c) {
    if (!ph_mqtt_owner() || c != port.client || !ph_mqtt_protocol_begin(&port.core))
        return MQTT_FAILURE;
    ph_mqtt_record_operation(&port.core, PH_MQTT_ERROR_SESSION);
    return end(PH_MQTT_ERROR_SESSION, MQTTDisconnect(c));
}

int ph_mqtt_subscribe(Client *c, const char *filter, enum QoS qos, messageHandler handler) {
    if (!filter || !handler || !begin(c, PH_MQTT_ERROR_SUBSCRIBE))
        return MQTT_FAILURE;
    unsigned at = 0;
    while (at < MAX_MESSAGE_HANDLERS && port.subscriptions[at].filter && strcmp(port.subscriptions[at].filter, filter))
        at++;
    int rc = at == MAX_MESSAGE_HANDLERS ? MQTT_FAILURE : MQTTSubscribe(c, filter, qos, ingress);
    if (rc == MQTT_SUCCESS) {
        port.subscriptions[at].filter = filter;
        port.subscriptions[at].handler = handler;
    }
    return end(PH_MQTT_ERROR_SUBSCRIBE, rc);
}

int ph_mqtt_yield(Client *c, int timeout) {
    if (!begin(c, PH_MQTT_ERROR_YIELD))
        return MQTT_FAILURE;
    return end(PH_MQTT_ERROR_YIELD, MQTTYield(c, timeout));
}

int ph_mqtt_publish(Client *c, const char *topic, MQTTMessage *m) {
    if (!m || !topic || !m->payload || m->payloadlen > PH_MQTT_PAYLOAD_BYTES || strnlen(topic, PH_MQTT_TOPIC_BYTES + 1) > PH_MQTT_TOPIC_BYTES || !ph_mqtt_owner())
        return MQTT_FAILURE;
    if (port.turn_active) {
        if (!port.turn_left)
            return PH_MQTT_MORE;
        --port.turn_left;
    }
    size_t packet = strlen(topic) + m->payloadlen + 9;
    /* SDK serializes a second copy, while callback RX can coexist. Its own
     * allocation is still fallible; this check is a soft live-heap admission. */
    if (!permit(NULL, packet + 32)) {
        port.cause = PH_MQTT_CAUSE_HEAP_ADMISSION;
        port.packet_bytes = packet;
        memset(&port.read_evidence, 0, sizeof(port.read_evidence));
        memset(&port.write_evidence, 0, sizeof(port.write_evidence));
        ph_mqtt_error(PH_MQTT_ERROR_PUBLISH, MQTT_FAILURE);
        port.failed = true;
        return MQTT_FAILURE;
    }
    if (!begin(c, PH_MQTT_ERROR_PUBLISH))
        return MQTT_FAILURE;
    port.packet_bytes = packet;
    int rc = end(PH_MQTT_ERROR_PUBLISH, MQTTPublish(c, topic, m));
    if (rc != MQTT_SUCCESS)
        port.failed = true;
    return rc;
}

void ph_mqtt_turn_begin(unsigned limit) {
    if (ph_mqtt_owner() && !port.core.protocol_depth && !port.core.dispatch_depth) {
        port.turn_active = true;
        port.turn_left = limit;
    }
}

void ph_mqtt_turn_end(void) {
    if (ph_mqtt_owner())
        port.turn_active = false;
}

uint32_t ph_mqtt_send_generation(void) {
    if (!initialized())
        return 0;
    mutex_lock(NULL);
    uint32_t generation = port.bound && port.core.accepting ? port.core.generation : 0;
    mutex_unlock(NULL);
    return generation;
}

int ph_mqtt_enqueue_send_generation(const char *topic, const void *payload, size_t n, unsigned qos, bool retain, uint32_t generation) {
    if (!initialized() || !topic || !generation)
        return MQTT_FAILURE;
    return ph_mqtt_enqueue_generation(&port.core, PH_MQTT_SEND, topic, strnlen(topic, PH_MQTT_TOPIC_BYTES + 1), payload, n, qos, retain, 0, generation) ? MQTT_SUCCESS : MQTT_FAILURE;
}

int ph_mqtt_enqueue_send(const char *topic, const void *payload, size_t n, unsigned qos, bool retain) {
    if (!initialized() || !topic)
        return MQTT_FAILURE;
    return ph_mqtt_enqueue(&port.core, PH_MQTT_SEND, topic, strnlen(topic, PH_MQTT_TOPIC_BYTES + 1), payload, n, qos, retain) ? MQTT_SUCCESS : MQTT_FAILURE;
}

static int send_emergency(Client *c) {
    for (unsigned i = 0; i < 2; i++)
        if (port.busy[i].used) {
        char topic[MQTT_TOPIC_MAX], wire[SERVICE_BUSY_BYTES];
        if (!mqtt_target_leaf(&port.busy[i].target, "result", topic, sizeof(topic)))
            return MQTT_FAILURE;
        service_busy_format(port.busy[i].id, wire);
        port.busy[i].used = false;
        MQTTMessage m = {0};
        m.qos = QOS1;
        m.payload = wire;
        m.payloadlen = strlen(wire);
        int rc = ph_mqtt_publish(c, topic, &m);
        if (rc != MQTT_SUCCESS)
            return rc;
    }
    return MQTT_SUCCESS;
}

bool ph_mqtt_input_consumed(void) {
    if (!ph_mqtt_owner() || !port.core.dispatch_depth || port.core.protocol_depth)
        return false;
    ph_mqtt_item_release(&port.core, port.core.inflight[PH_MQTT_COMMAND]);
    return ph_mqtt_credit_replace(&port.core, PH_MQTT_RECEIPT_BYTES);
}

bool ph_mqtt_work_pending(void) {
    if (!ph_mqtt_owner() || port.core.protocol_depth || port.core.dispatch_depth || port.failed)
        return false;
    mutex_lock(NULL);
    bool pending = port.core.head[PH_MQTT_COMMAND] || port.core.head[PH_MQTT_SEND];
    mutex_unlock(NULL);
    /* Emergency slots are written/retired only by this owner. */
    return pending || port.busy[0].used || port.busy[1].used;
}

int ph_mqtt_poll(Client *c) {
    if (!c)
        c = port.client;
    if (!ph_mqtt_owner() || c != port.client || port.core.protocol_depth || port.core.dispatch_depth)
        return MQTT_FAILURE;
    if (port.failed)
        return MQTT_FAILURE;
    int rc = send_emergency(c);
    if (rc != MQTT_SUCCESS)
        return rc;
    /* Fixed budgets prevent callbacks arriving during PUBACK from extending
     * this pass. One business command, then at most one external telemetry. */
    ph_mqtt_item *i = ph_mqtt_pop(&port.core, PH_MQTT_COMMAND);
    if (i) {
        if (!ph_mqtt_credit_acquire(&port.core, SERVICE_BUSY_BYTES)) {
            /* Even the fixed failure receipt must have credit before dispatch. */
            if (modern(i->data, i->topic_length)) {
                char topic[MQTT_TOPIC_MAX], wire[SERVICE_BUSY_BYTES];
                mqtt_target_t target;
                mqtt_target_set(&target, port.account);
                mqtt_target_leaf(&target, "result", topic, sizeof(topic));
                service_busy_wire(ph_mqtt_item_payload(i), i->length, wire);
                MQTTMessage m = {0};
                m.qos = QOS1;
                m.payload = wire;
                m.payloadlen = strlen(wire);
                rc = ph_mqtt_publish(c, topic, &m);
                ph_mqtt_item_release(&port.core, i);
                if (rc != MQTT_SUCCESS)
                    return rc;
                goto send_one;
            }
            /* With no outstanding owner credit, acquire failed only because
             * the shared ledger was full. This legacy/nonmodern input has not
             * run and has no modern busy protocol: count/drop it, not the
             * healthy connection. A preexisting credit remains an invariant
             * failure. Only this owner mutates credit; producers cannot race it. */
            if (!port.core.credit) {
                if (!i->retained)
                    ph_mqtt_record_ingress_drop(&port.core, false);
                ph_mqtt_item_release(&port.core, i);
                goto send_one;
            }
            ph_mqtt_item_release(&port.core, i);
            return ph_mqtt_app_error(PH_MQTT_ERROR_APP_RECEIPT_ADMISSION, MQTT_FAILURE);
        }
        MQTTString topic = MQTTString_initializer;
        topic.cstring = i->data;
        MQTTMessage m = {0};
        m.payload = (void *) ph_mqtt_item_payload(i);
        m.payloadlen = i->length;
        m.qos = i->qos;
        m.retained = i->retained;
        MessageData d = { &m, &topic};
        if (i->generation == port.core.generation && ph_mqtt_dispatch_begin(&port.core)) {
            port.subscriptions[i->route].handler(&d);
            ph_mqtt_dispatch_end(&port.core);
        }
        /* A modern handler may consume its borrowed entry after parsing.
         * Never touch that old MessageData/topic/payload after dispatch. */
        if (port.core.inflight[PH_MQTT_COMMAND])
            ph_mqtt_item_release(&port.core, port.core.inflight[PH_MQTT_COMMAND]);
        ph_mqtt_credit_release(&port.core);
        if (port.failed)
            return MQTT_FAILURE;
    }
send_one:
    /* A refused COMMAND consumes its one attempt, not its external SEND turn. */
    i = ph_mqtt_pop(&port.core, PH_MQTT_SEND);
    if (i) {
        MQTTMessage m = {0};
        m.payload = (void *) ph_mqtt_item_payload(i);
        m.payloadlen = i->length;
        m.qos = i->qos;
        m.retained = i->retained;
        if (i->generation == port.core.generation)
            rc = port.outgoing ? port.outgoing(i) : ph_mqtt_publish(c, i->data, &m);
        ph_mqtt_item_release(&port.core, i);
    }
    return rc;
}
