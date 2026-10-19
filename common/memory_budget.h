#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reservations are conservative: in-flight allocations also reduce free heap.
 * Keep system/network headroom, and serialize competing JSON builders when
 * their estimates no longer fit. This is admission control, not an allocator. */
#define SERVICE_MEMORY_RESERVE 16384u
#define SERVICE_MEMORY_WORK 12288u
typedef struct {
    uint32_t reserved, active, denied, waiting, minimum_free;
} service_memory_state_t;
static inline unsigned service_memory_cost(size_t input) {
    return input <= 8192 ? SERVICE_MEMORY_WORK + (unsigned)input * 2u : 0;
}
static inline unsigned memory_budget_claim(service_memory_state_t *s, uint32_t free_bytes, size_t input) {
    unsigned cost = service_memory_cost(input);
    if (free_bytes < s->minimum_free) s->minimum_free = free_bytes;
    if (!cost || free_bytes < SERVICE_MEMORY_RESERVE ||
        cost > free_bytes - SERVICE_MEMORY_RESERVE ||
        s->reserved > free_bytes - SERVICE_MEMORY_RESERVE - cost) {
        if (s->denied != UINT32_MAX) s->denied++;
        return 0;
    }
    s->reserved += cost; s->active++;
    return cost;
}
static inline void memory_budget_release(service_memory_state_t *s, unsigned cost) {
    if (cost && cost <= s->reserved && s->active) { s->reserved -= cost; s->active--; }
}
