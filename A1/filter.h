#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

/* Product estimate: 180 days of continuous operation; user-adjustable. */
#define A1_FILTER_MAGIC 0x41314631u
#define A1_FILTER_DEFAULT_HOURS 4320u
#define A1_FILTER_MAX_HOURS 20000u
#define A1_FILTER_CHECKPOINT_SECONDS 3600u
#define A1_FILTER_MQTT_INTERVAL_MS 60000u

typedef struct {
    uint32_t magic, used_seconds, lifetime_hours;
} a1_filter_storage_t;

typedef struct {
    uint32_t last_ms, remainder_ms, saved_seconds;
    bool initialized, running;
} a1_filter_clock_t;

static inline void a1_filter_defaults(a1_filter_storage_t *f)
{
    *f = (a1_filter_storage_t){A1_FILTER_MAGIC, 0, A1_FILTER_DEFAULT_HOURS};
}

/* Unsigned elapsed time handles the MiCO millisecond clock wrapping. The
 * previous output state owns the interval ending at this observation. */
static inline void a1_filter_advance(a1_filter_storage_t *f, a1_filter_clock_t *c,
                                     uint32_t now, bool running)
{
    if (c->initialized && c->running) {
        uint64_t elapsed = (uint32_t)(now - c->last_ms) + (uint64_t)c->remainder_ms;
        uint64_t seconds = f->used_seconds + elapsed / 1000u;
        f->used_seconds = seconds > INT_MAX ? INT_MAX : (uint32_t)seconds;
        c->remainder_ms = elapsed % 1000u;
    }
    c->last_ms = now; c->running = running; c->initialized = true;
}

static inline unsigned a1_filter_remaining(const a1_filter_storage_t *f)
{
    uint32_t total = f->lifetime_hours * 3600u;
    if (!total || f->used_seconds >= total) return 0;
    /* Keep 0% for exhaustion, rounding partial percentage points upward. */
    return (unsigned)(((uint64_t)(total - f->used_seconds) * 100u + total - 1) / total);
}

/* Reset and lifetime edits must reach other subscribers without a status poll.
 * Accumulating seconds are coalesced to avoid publishing on every fan tick. */
static inline bool a1_filter_mqtt_due(const a1_filter_storage_t *current,
                                     const a1_filter_storage_t *sent,
                                     uint32_t now, uint32_t sent_at)
{
    return current->lifetime_hours != sent->lifetime_hours ||
        current->used_seconds < sent->used_seconds ||
        (current->used_seconds != sent->used_seconds &&
         (uint32_t)(now - sent_at) >= A1_FILTER_MQTT_INTERVAL_MS);
}
