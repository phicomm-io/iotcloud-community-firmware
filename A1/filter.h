#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#define A1_FILTER_MAGIC 0x464c5431u
#define A1_FILTER_DEFAULT_HOURS 6000u
typedef struct { uint32_t magic, used_seconds, lifetime_hours; } a1_filter_config_t;
typedef struct { uint32_t at, remainder; bool initialized, running; } a1_filter_clock_t;

/* Count elapsed time in the previous applied motor state; RTOS wrap is safe. */
static inline void a1_filter_advance(a1_filter_config_t *c, a1_filter_clock_t *clock,
                                      uint32_t now, bool running)
{
    if (clock->initialized && clock->running) {
        uint64_t ms = (uint32_t)(now - clock->at) + (uint64_t)clock->remainder;
        uint64_t used = c->used_seconds + ms / 1000;
        c->used_seconds = used > UINT32_MAX ? UINT32_MAX : (uint32_t)used;
        clock->remainder = ms % 1000;
    }
    clock->at = now;
    clock->running = running;
    clock->initialized = true;
}

void a1_filter_tick(uint32_t now, bool running);
void a1_filter_get(a1_filter_config_t *out);
bool a1_filter_update(bool reset, unsigned lifetime_hours);
