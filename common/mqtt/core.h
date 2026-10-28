/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_MQTT_CORE_H
#define PH_MQTT_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Payload storage is demand allocated. These are ceilings, not reserved RAM
 * and not a partition of the closed network heap. Both queues and the popped
 * in-flight entry remain charged until their owner releases them. */
#define PH_MQTT_COMMAND_COUNT 4u
#define PH_MQTT_SEND_COUNT 6u
#define PH_MQTT_QUEUE_BYTES 12288u
#define PH_MQTT_TOPIC_BYTES 511u
#define PH_MQTT_PAYLOAD_BYTES 8192u
#define PH_MQTT_RECEIPT_BYTES 8192u
/* Outbound producers cannot occupy the bytes reserved for one result. */
#define PH_MQTT_SEND_BYTES (PH_MQTT_QUEUE_BYTES-PH_MQTT_RECEIPT_BYTES)
enum {
    PH_MQTT_COMMAND, PH_MQTT_SEND
};
typedef struct ph_mqtt_item ph_mqtt_item;
struct ph_mqtt_item {
    ph_mqtt_item *next;
    uint32_t queued_at, generation;
    size_t charge, length;
    uint16_t topic_length, route;
    uint8_t kind, qos;
    bool retained;
    /* NUL-terminated topic followed by length bytes and one trailing NUL.
     * The terminators do not turn arbitrary MQTT payload into a C string. */
    char data[];
};
typedef struct {
    void *context;
    void(*lock)(void *);
    void(*unlock)(void *);
    bool(*owner)(void *);
    uint32_t(*now)(void *);
    bool(*permit)(void *, size_t);
    void * (*allocate)(void *, size_t);
    void(*release)(void *, void *);
}

ph_mqtt_hooks;
/* Numeric evidence only. Zero read means the adapter returned no bytes;
 * negative read does not identify select/recv/peer-close in an opaque adapter. */
typedef struct {
    int32_t requested, returned;
    uint32_t timeout_ms, elapsed_ms;
} ph_mqtt_io_evidence;
enum {
    PH_MQTT_CAUSE_UNKNOWN, PH_MQTT_CAUSE_SDK, PH_MQTT_CAUSE_HEAP_ADMISSION,
    PH_MQTT_CAUSE_RX_FRAMING, PH_MQTT_CAUSE_RECEIPT_EXHAUSTED,
    PH_MQTT_CAUSE_READ, PH_MQTT_CAUSE_WRITE, PH_MQTT_CAUSE_RESOURCE_EXPIRED
};
typedef struct {
    uint32_t reason, operation, free_bytes, packet_bytes, credit_bytes, queue_bytes, rx_state;
    ph_mqtt_io_evidence read, write;
} ph_mqtt_failure_evidence;
typedef struct {
    ph_mqtt_hooks hooks;
    ph_mqtt_item *head[2], *tail[2], *inflight[2];
    unsigned count[2], generation;
    size_t bytes, peak_bytes, credit, kind_bytes[2];
    size_t rx_buffer_bytes, tx_buffer_bytes, rx_peak_bytes, tx_peak_bytes;
    uint32_t accepted[2], rejected[2], owner_faults, max_delay;
    uint32_t ingress_dropped, busy_overflow;
    unsigned protocol_depth, dispatch_depth, operation;
    int last_error;
    unsigned last_stage;
    uint32_t errors, reconnects;
    bool accepting, error_latched;
    ph_mqtt_failure_evidence failure;
}

ph_mqtt_core;
typedef struct {
    unsigned queued[2], inflight[2], generation;
    size_t bytes, peak_bytes, credit;
    size_t rx_buffer_bytes, tx_buffer_bytes, rx_peak_bytes, tx_peak_bytes;
    unsigned protocol_depth, operation, last_stage;
    int last_error;
    uint32_t errors, reconnects;
    uint32_t rejected[2], owner_faults, max_delay_ms;
    uint32_t ingress_dropped, busy_overflow;
    ph_mqtt_failure_evidence failure;
}

ph_mqtt_diagnostics;
/* Short-lock POD only: no borrowed queue pointers or sensitive bytes. */
void ph_mqtt_core_snapshot(ph_mqtt_core *, ph_mqtt_diagnostics *);
/* Unadmitted non-retained input only; no business execution or receipt promise. */
void ph_mqtt_record_ingress_drop(ph_mqtt_core *, bool);
void ph_mqtt_record_operation(ph_mqtt_core *, unsigned);
/* Begin preserves the previous failed session cause; events always count,
 * but only the first cause-bearing event in this session replaces it. */
void ph_mqtt_error_session_begin(ph_mqtt_core *);
void ph_mqtt_record_error(ph_mqtt_core *, unsigned, int, bool);
void ph_mqtt_record_failure(ph_mqtt_core *, unsigned, unsigned, int, uint32_t, unsigned, unsigned,
                            const ph_mqtt_io_evidence *, const ph_mqtt_io_evidence *);
void ph_mqtt_record_reconnect(ph_mqtt_core *);
void ph_mqtt_protocol_buffers(ph_mqtt_core *, size_t, size_t);
bool ph_mqtt_core_init(ph_mqtt_core *, const ph_mqtt_hooks *);
/* Rebind an empty, quiescent owner. Published hooks remain immutable: readers
 * and producers need their lock callback before they can acquire that lock. */
bool ph_mqtt_core_reset(ph_mqtt_core *);
bool ph_mqtt_is_owner(ph_mqtt_core *);
bool ph_mqtt_protocol_begin(ph_mqtt_core *);
void ph_mqtt_protocol_end(ph_mqtt_core *);
bool ph_mqtt_enqueue(ph_mqtt_core *, unsigned, const void *, size_t, const void *, size_t, unsigned, bool);
bool ph_mqtt_enqueue_route(ph_mqtt_core *, unsigned, const void *, size_t, const void *, size_t, unsigned, bool, unsigned);
bool ph_mqtt_enqueue_generation(ph_mqtt_core *, unsigned, const void *, size_t, const void *, size_t, unsigned, bool, unsigned, uint32_t);
/* Owner rollback of a new subscription: only queued input for this route. */
void ph_mqtt_discard_route(ph_mqtt_core *, unsigned);
ph_mqtt_item *ph_mqtt_pop(ph_mqtt_core *, unsigned);
void ph_mqtt_item_release(ph_mqtt_core *, ph_mqtt_item *);
/* Single owner credit; no allocation, and never a substitute for checking an
 * actual JSON/native allocation. Hold through response send, then return it. */
bool ph_mqtt_credit_acquire(ph_mqtt_core *, size_t);
bool ph_mqtt_credit_replace(ph_mqtt_core *, size_t);
void ph_mqtt_credit_release(ph_mqtt_core *);
bool ph_mqtt_dispatch_begin(ph_mqtt_core *);
void ph_mqtt_dispatch_end(ph_mqtt_core *);
bool ph_mqtt_clear(ph_mqtt_core *);
void ph_mqtt_core_accepting(ph_mqtt_core *, bool);
static inline const char *ph_mqtt_item_topic(const ph_mqtt_item *i) {
    return i->data;
}

static inline const void *ph_mqtt_item_payload(const ph_mqtt_item *i) {
    return i->data + i->topic_length + 1;
}
/* Streaming framing guard runs BEFORE the pinned library can realloc for a
 * received Remaining Length. Failure poisons this connection until teardown;
 * never skip an oversized packet and continue out of sync. */
typedef struct {
    uint32_t remaining, multiplier, left, serial;
    unsigned length_bytes, state;
    size_t maximum;
    bool failed;
}

ph_mqtt_rx;
void ph_mqtt_rx_init(ph_mqtt_rx *, size_t);
bool ph_mqtt_rx_feed(ph_mqtt_rx *, const unsigned char *, size_t);
#endif
