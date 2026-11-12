/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core.h"
#include <string.h>
#include <limits.h>
static void lock(ph_mqtt_core *s) {
    s->hooks.lock(s->hooks.context);
}

static void unlock(ph_mqtt_core *s) {
    s->hooks.unlock(s->hooks.context);
}

static void increment(uint32_t *v) {
    if (*v != UINT32_MAX)
        ++ *v;
}

bool ph_mqtt_core_init(ph_mqtt_core *s, const ph_mqtt_hooks *h) {
    if (!s || !h || !h->lock || !h->unlock || !h->owner || !h->now || !h->permit || !h->allocate || !h->release)
        return false;
    memset(s, 0, sizeof(*s));
    s->hooks = *h;
    s->generation = 1;
    s->accepting = true;
    return true;
}

bool ph_mqtt_core_reset(ph_mqtt_core *s) {
    if (!ph_mqtt_is_owner(s))
        return false;
    lock(s);
    if (s->protocol_depth || s->dispatch_depth || s->count[0] || s->count[1] || s->credit || s->inflight[0] || s->inflight[1]) {
        unlock(s);
        return false;
    }
    uint32_t generation = s->generation + 1;
    if (!generation)
        ++generation;
    /* Never overwrite hooks after initial publication, including while holding
     * their mutex. A concurrent caller first reads hooks.lock to enter it. */
    memset((char *) s + offsetof(ph_mqtt_core, head), 0, sizeof(*s) - offsetof(ph_mqtt_core, head));
    s->generation = generation;
    unlock(s);
    return true;
}

bool ph_mqtt_is_owner(ph_mqtt_core *s) {
    if (s->hooks.owner(s->hooks.context))
        return true;
    lock(s);
    increment(&s->owner_faults);
    unlock(s);
    return false;
}

bool ph_mqtt_protocol_begin(ph_mqtt_core *s) {
    if (!ph_mqtt_is_owner(s) || s->protocol_depth)
        return false;
    lock(s);
    ++s->protocol_depth;
    unlock(s);
    return true;
}

void ph_mqtt_protocol_end(ph_mqtt_core *s) {
    if (ph_mqtt_is_owner(s) && s->protocol_depth) {
        lock(s);
        --s->protocol_depth;
        unlock(s);
    }
}

bool ph_mqtt_enqueue(ph_mqtt_core *s, unsigned kind, const void *topic, size_t tn, const void *payload, size_t n, unsigned qos, bool retained) {
    return ph_mqtt_enqueue_route(s, kind, topic, tn, payload, n, qos, retained, 0);
}

bool ph_mqtt_enqueue_route(ph_mqtt_core *s, unsigned kind, const void *topic, size_t tn, const void *payload, size_t n, unsigned qos, bool retained, unsigned route) {
    return ph_mqtt_enqueue_generation(s, kind, topic, tn, payload, n, qos, retained, route, 0);
}

bool ph_mqtt_enqueue_generation(ph_mqtt_core *s, unsigned kind, const void *topic, size_t tn, const void *payload, size_t n, unsigned qos, bool retained, unsigned route, uint32_t generation) {
    if (kind > PH_MQTT_SEND || !topic || !tn || tn > PH_MQTT_TOPIC_BYTES || (!payload && n) || n > PH_MQTT_PAYLOAD_BYTES || qos > 1 || route > UINT16_MAX || memchr(topic, 0, tn))
        return false;
    size_t size = sizeof(ph_mqtt_item) + tn + 1 + n + 1;
    /* Include allocator header/alignment in the admission ledger. */
    size_t charge = (size + 31u) &~(size_t) 15u;
    lock(s);
    unsigned cap = kind == PH_MQTT_COMMAND ? PH_MQTT_COMMAND_COUNT : PH_MQTT_SEND_COUNT;
    /* Only queued retained telemetry coalesces. Never replace an admitted
     * command, in-flight item, or ordered non-retained receipt. */
    ph_mqtt_item *replace = NULL, *previous = NULL;
    if (kind == PH_MQTT_SEND && retained) {
        ph_mqtt_item *prior = NULL;
        for (ph_mqtt_item *at = s->head[kind]; at; prior = at, at = at->next)
            if (at->retained && at->qos == qos && at->topic_length == tn && !memcmp(at->data, topic, tn)) {
            replace = at;
            previous = prior;
            break;
        }
    }
    /* Admission includes the temporary old+new overlap during replacement. */
    if (!s->accepting || (generation && generation != s->generation) || (!replace && s->count[kind] >= cap) || charge > PH_MQTT_QUEUE_BYTES || (kind == PH_MQTT_SEND && (charge > PH_MQTT_SEND_BYTES || s->kind_bytes[kind] > PH_MQTT_SEND_BYTES - charge)) || s->bytes + s->credit > PH_MQTT_QUEUE_BYTES - charge || !s->hooks.permit(s->hooks.context, charge)) {
        increment(&s->rejected[kind]);
        unlock(s);
        return false;
    }
    ph_mqtt_item *i = s->hooks.allocate(s->hooks.context, size);
    if (!i) {
        increment(&s->rejected[kind]);
        unlock(s);
        return false;
    }
    memset(i, 0, sizeof(*i));
    i->kind = kind;
    i->qos = qos;
    i->retained = retained;
    i->length = n;
    i->topic_length = tn;
    i->route = route;
    i->charge = charge;
    i->queued_at = s->hooks.now(s->hooks.context);
    i->generation = s->generation;
    memcpy(i->data, topic, tn);
    i->data[tn] = 0;
    if (n)
        memcpy(i->data + tn + 1, payload, n);
    i->data[tn + 1 + n] = 0;
    s->bytes += charge;
    s->kind_bytes[kind] += charge;
    if (s->bytes > s->peak_bytes)
        s->peak_bytes = s->bytes;
    if (replace) {
        i->next = replace->next;
        if (previous)
            previous->next = i;
        else s->head[kind] = i;
        if (s->tail[kind] == replace)
            s->tail[kind] = i;
        s->bytes -= replace->charge;
        s->kind_bytes[kind] -= replace->charge;
        s->hooks.release(s->hooks.context, replace);
    } else {
        if (s->tail[kind])
            s->tail[kind]->next = i;
        else s->head[kind] = i;
        s->tail[kind] = i;
        ++s->count[kind];
    }
    increment(&s->accepted[kind]);
    unlock(s);
    return true;
}

void ph_mqtt_core_snapshot(ph_mqtt_core *s, ph_mqtt_diagnostics *out) {
    lock(s);
    memset(out, 0, sizeof(*out));
    for (unsigned k = 0; k < 2; k++) {
        out->inflight[k] = s->inflight[k] != NULL;
        out->queued[k] = s->count[k] - out->inflight[k];
        out->rejected[k] = s->rejected[k];
    }
    out->bytes = s->bytes;
    out->peak_bytes = s->peak_bytes;
    out->credit = s->credit;
    out->generation = s->generation;
    out->owner_faults = s->owner_faults;
    out->ingress_dropped = s->ingress_dropped;
    out->busy_overflow = s->busy_overflow;
    out->max_delay_ms = s->max_delay;
    out->rx_buffer_bytes = s->rx_buffer_bytes;
    out->tx_buffer_bytes = s->tx_buffer_bytes;
    out->rx_peak_bytes = s->rx_peak_bytes;
    out->tx_peak_bytes = s->tx_peak_bytes;
    out->protocol_depth = s->protocol_depth;
    out->operation = s->operation;
    out->last_stage = s->last_stage;
    out->failure = s->failure;
    out->last_error = s->last_error;
    out->errors = s->errors;
    out->reconnects = s->reconnects;
    unlock(s);
}

void ph_mqtt_record_ingress_drop(ph_mqtt_core *s, bool busy_overflow) {
    if (!ph_mqtt_is_owner(s))
        return;
    lock(s);
    increment(&s->ingress_dropped);
    if (busy_overflow)
        increment(&s->busy_overflow);
    unlock(s);
}

void ph_mqtt_record_operation(ph_mqtt_core *s, unsigned stage) {
    if (!ph_mqtt_is_owner(s))
        return;
    lock(s);
    s->operation = stage;
    unlock(s);
}

void ph_mqtt_error_session_begin(ph_mqtt_core *s) {
    if (!ph_mqtt_is_owner(s))
        return;
    lock(s);
    s->error_latched = false;
    s->operation = 0; /* A source tag belongs only to the immediately failing session. */
    unlock(s);
}

void ph_mqtt_record_failure(ph_mqtt_core *s, unsigned reason, unsigned operation, int rc,
                            uint32_t free_bytes, unsigned packet_bytes, unsigned rx_state,
                            const ph_mqtt_io_evidence *read, const ph_mqtt_io_evidence *write) {
    if (!ph_mqtt_is_owner(s)) return;
    lock(s);
    if (!s->error_latched) {
        s->failure.reason = reason;
        s->failure.operation = operation;
        s->failure.free_bytes = free_bytes;
        s->failure.packet_bytes = packet_bytes;
        s->failure.rx_state = rx_state;
        s->failure.read = *read;
        s->failure.write = *write;
        s->failure.credit_bytes = (uint32_t)s->credit;
        s->failure.queue_bytes = (uint32_t)s->bytes;
        s->last_stage = operation;
        s->last_error = rc;
        s->error_latched = true;
    }
    increment(&s->errors);
    unlock(s);
}

void ph_mqtt_record_error(ph_mqtt_core *s, unsigned stage, int rc, bool cause) {
    if (!ph_mqtt_is_owner(s))
        return;
    lock(s);
    if (cause && !s->error_latched) {
        s->last_stage = stage;
        s->last_error = rc;
        s->error_latched = true;
    }
    increment(&s->errors);
    unlock(s);
}

void ph_mqtt_record_reconnect(ph_mqtt_core *s) {
    if (!ph_mqtt_is_owner(s))
        return;
    lock(s);
    increment(&s->reconnects);
    unlock(s);
}

void ph_mqtt_protocol_buffers(ph_mqtt_core *s, size_t rx, size_t tx) {
    if (!ph_mqtt_is_owner(s))
        return;
    lock(s);
    s->rx_buffer_bytes = rx;
    s->tx_buffer_bytes = tx;
    if (rx > s->rx_peak_bytes)
        s->rx_peak_bytes = rx;
    if (tx > s->tx_peak_bytes)
        s->tx_peak_bytes = tx;
    unlock(s);
}

void ph_mqtt_discard_route(ph_mqtt_core *s, unsigned route) {
    if (!ph_mqtt_is_owner(s) || s->dispatch_depth)
        return;
    lock(s);
    bool accepting = s->accepting;
    s->accepting = false;
    ph_mqtt_item *discard = NULL;
    ph_mqtt_item **next = &s->head[PH_MQTT_COMMAND], *previous = NULL;
    while (*next) {
        ph_mqtt_item *i = *next;
        if (i->route != route) {
            previous = i;
            next = &i->next;
            continue;
        }
        *next = i->next;
        if (s->tail[PH_MQTT_COMMAND] == i)
            s->tail[PH_MQTT_COMMAND] = previous;
        s->bytes -= i->charge;
        s->kind_bytes[PH_MQTT_COMMAND] -= i->charge;
        --s->count[PH_MQTT_COMMAND];
        i->next = discard;
        discard = i;
    }
    unlock(s);
    /* As in clear(), keep admissions closed across detached allocations. */
    while (discard) {
        ph_mqtt_item *i = discard;
        discard = i->next;
        s->hooks.release(s->hooks.context, i);
    }
    lock(s);
    s->accepting = accepting;
    unlock(s);
}

ph_mqtt_item *ph_mqtt_pop(ph_mqtt_core *s, unsigned kind) {
    if (kind > PH_MQTT_SEND || !ph_mqtt_is_owner(s) || s->protocol_depth)
        return NULL;
    lock(s);
    ph_mqtt_item *i = s->inflight[kind] ? NULL : s->head[kind];
    if (i) {
        s->head[kind] = i->next;
        if (!s->head[kind])
            s->tail[kind] = NULL;
        i->next = NULL;
        s->inflight[kind] = i;
        uint32_t delay = s->hooks.now(s->hooks.context) - i->queued_at;
        if (delay > s->max_delay)
            s->max_delay = delay;
    }
    /* count includes a popped in-flight item, just like bytes. */
    unlock(s);
    return i;
}

void ph_mqtt_item_release(ph_mqtt_core *s, ph_mqtt_item *i) {
    if (!i || !ph_mqtt_is_owner(s))
        return;
    lock(s);
    unsigned kind = s->inflight[0] == i ? 0 : s->inflight[1] == i ? 1 : 2;
    if (kind == 2) {
        unlock(s);
        return;
    }
    s->inflight[kind] = NULL;
    s->bytes -= i->charge;
    s->kind_bytes[kind] -= i->charge;
    --s->count[kind];
    s->hooks.release(s->hooks.context, i);
    unlock(s);
}

bool ph_mqtt_credit_acquire(ph_mqtt_core *s, size_t bytes) {
    if (!ph_mqtt_is_owner(s) || s->protocol_depth)
        return false;
    lock(s);
    bool ok = !s->credit && bytes <= PH_MQTT_QUEUE_BYTES && s->bytes <= PH_MQTT_QUEUE_BYTES - bytes;
    if (ok)
        s->credit = bytes;
    unlock(s);
    return ok;
}

bool ph_mqtt_credit_replace(ph_mqtt_core *s, size_t bytes) {
    if (!ph_mqtt_is_owner(s) || s->protocol_depth)
        return false;
    lock(s);
    bool ok = bytes <= PH_MQTT_QUEUE_BYTES && s->bytes <= PH_MQTT_QUEUE_BYTES - bytes;
    if (ok)
        s->credit = bytes;
    unlock(s);
    return ok;
}

void ph_mqtt_credit_release(ph_mqtt_core *s) {
    if (!ph_mqtt_is_owner(s))
        return;
    lock(s);
    s->credit = 0;
    unlock(s);
}

bool ph_mqtt_dispatch_begin(ph_mqtt_core *s) {
    if (!ph_mqtt_is_owner(s) || s->protocol_depth || s->dispatch_depth)
        return false;
    ++s->dispatch_depth;
    return true;
}

void ph_mqtt_dispatch_end(ph_mqtt_core *s) {
    if (ph_mqtt_is_owner(s) && s->dispatch_depth)
        --s->dispatch_depth;
}

void ph_mqtt_core_accepting(ph_mqtt_core *s, bool accepting) {
    if (!ph_mqtt_is_owner(s))
        return;
    lock(s);
    s->accepting = accepting;
    unlock(s);
}

bool ph_mqtt_clear(ph_mqtt_core *s) {
    if (!ph_mqtt_is_owner(s) || s->protocol_depth || s->dispatch_depth || s->inflight[0] || s->inflight[1])
        return false;
    /* Detach both finite lists and change generation in one critical section.
     * Admissions stay closed until every detached allocation is released, so
     * old and new generations cannot each consume the entire byte budget. */
    lock(s);
    bool accepting = s->accepting;
    s->accepting = false;
    ph_mqtt_item *old[2] = {s->head[0], s->head[1]};
    memset(s->head, 0, sizeof(s->head));
    memset(s->tail, 0, sizeof(s->tail));
    memset(s->count, 0, sizeof(s->count));
    s->bytes = 0;
    s->credit = 0;
    memset(s->kind_bytes, 0, sizeof(s->kind_bytes));
    if (!++s->generation)
        ++s->generation;
    unlock(s);
    for (unsigned kind = 0; kind < 2; kind++)
        while (old[kind]) {
        ph_mqtt_item *i = old[kind];
        old[kind] = i->next;
        s->hooks.release(s->hooks.context, i);
    }
    lock(s);
    s->accepting = accepting;
    unlock(s);
    return true;
}

void ph_mqtt_rx_init(ph_mqtt_rx *r, size_t maximum) {
    memset(r, 0, sizeof(*r));
    r->maximum = maximum;
}

bool ph_mqtt_rx_feed(ph_mqtt_rx *r, const unsigned char *p, size_t n) {
    if (r->failed || (!p && n))
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned c = p[i];
        if (!r->state) {
            if (c < 0x10 || c >= 0xf0)
                goto fail;
            r->state = 1;
            r->remaining = 0;
            r->multiplier = 1;
            r->length_bytes = 0;
            ++r->serial;
        } else if (r->state == 1) {
            if (++r->length_bytes > 4)
                goto fail;
            r->remaining +=(c &127) *r->multiplier;
            if (c &128) {
                if (r->length_bytes == 4)
                    goto fail;
                r->multiplier *= 128;
            } else {
                if ((r->length_bytes > 1 && r->remaining < r->multiplier) || r->remaining > r->maximum || r->remaining + 1 + r->length_bytes > r->maximum)
                    goto fail;
                r->left = r->remaining;
                r->state = r->left ? 2 : 0;
            }
        } else if (!--r->left) r->state = 0;
    }
    return true;
    fail : r->failed = true;
    return false;
}
