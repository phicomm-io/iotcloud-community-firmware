#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#define A1_FILTER_STOCK_MAGIC 0x41315731u
#define A1_FILTER_STOCK_CAPACITY 12000000u
/* Append-only extension; never enlarge the legacy 12-byte filter record. */
typedef struct {
    uint32_t magic, usage, air_litres, usage_remainder, air_remainder;
    uint8_t weighted;
    uint8_t reserved[3];
} a1_filter_stock_t;
typedef struct {
    uint32_t last_ms;
    unsigned speed, weight;
    bool initialized;
} a1_filter_stock_clock_t;
static inline bool a1_filter_stock_valid(const a1_filter_stock_t *s)
{
    return s->magic == A1_FILTER_STOCK_MAGIC && s->weighted <= 1 &&
        s->usage <= A1_FILTER_STOCK_CAPACITY && s->usage_remainder < 3600000u && s->air_remainder < 3600u;
}
static inline void a1_filter_stock_defaults(a1_filter_stock_t *s, bool weighted)
{ *s = (a1_filter_stock_t){.magic=A1_FILTER_STOCK_MAGIC,.weighted=weighted}; }
/* Community duty 1..100 is normalized to the OEM flow proxy 80..390.
 * It is an estimate, not a measured flow calibration. No RPM assumption. */
static inline unsigned a1_filter_flow(unsigned speed)
{ return speed ? 80u + (speed > 100 ? 100 : speed) * 310u / 100u : 0; }
static inline void a1_filter_stock_advance(a1_filter_stock_t *s, a1_filter_stock_clock_t *c,
                                         uint32_t now)
{
    if (c->initialized && c->speed) {
        uint32_t elapsed = now - c->last_ms;
        uint64_t flow_ms = (uint64_t)a1_filter_flow(c->speed) * elapsed;
        uint64_t usage = flow_ms * c->weight + s->usage_remainder;
        uint64_t total = s->usage + usage / 3600000u;
        s->usage = total > A1_FILTER_STOCK_CAPACITY ? A1_FILTER_STOCK_CAPACITY : total;
        s->usage_remainder = usage % 3600000u;
        uint64_t air = flow_ms + s->air_remainder;
        total = s->air_litres + air / 3600u;
        s->air_litres = total > UINT32_MAX ? UINT32_MAX : total;
        s->air_remainder = air % 3600u;
    }
    c->last_ms = now; c->initialized = true;
}
static inline unsigned a1_filter_stock_remaining(const a1_filter_stock_t *s)
{
    return s->usage >= A1_FILTER_STOCK_CAPACITY ? 0 :
        (unsigned)(((uint64_t)(A1_FILTER_STOCK_CAPACITY-s->usage)*100u + A1_FILTER_STOCK_CAPACITY-1)/A1_FILTER_STOCK_CAPACITY);
}
