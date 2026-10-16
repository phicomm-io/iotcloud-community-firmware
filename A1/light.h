#ifndef A1_LIGHT_H
#define A1_LIGHT_H
#include <stdint.h>
#include <stdbool.h>
#include "ring_feedback.h"

typedef struct {
    uint32_t started, duration, color;
    bool active;
} a1_key_light_t;

static inline void a1_key_light_start(a1_key_light_t *feedback, unsigned key,
                                     bool locked, bool hold, uint32_t now)
{
    feedback->started = now;
    feedback->duration = hold ? 160 : 300;
    /* Shared packed format is GGRRBB: power green, Wi-Fi blue, locked red. */
    feedback->color = locked ? 0x00ff00u : key == 0 ? 0xff0000u : 0x0000ffu;
    feedback->active = true;
}

static inline bool a1_key_light_frame(a1_key_light_t *feedback, uint32_t now,
                                      unsigned brightness, uint32_t colors[32])
{
    if (!feedback->active) return false;
    if ((uint32_t)(now - feedback->started) >= feedback->duration) {
        feedback->active = false;
        return false;
    }
    unsigned g = feedback->color >> 16 & 255, r = feedback->color >> 8 & 255;
    unsigned b = feedback->color & 255;
    uint32_t color = ((g * brightness / 255) << 16) |
        ((r * brightness / 255) << 8) | (b * brightness / 255);
    for (unsigned i = 0; i < 32; i++) colors[i] = color;
    return true;
}

#define A1_RING_POSITIONS 31u

/* Use the same physical LED-10 origin and percentage edge as touch feedback.
 * The wire-slot mapping belongs to ring_feedback.h, not a second renderer. */
static inline void a1_fan_frame(uint32_t colors[32], unsigned speed, uint32_t color)
{
    a1_ring_feedback(colors, speed, 255, color);
}
/* One-based physical mapping test; zero exits without changing business frame. */
static inline bool a1_test_frame(uint32_t colors[32], unsigned slot,
                                uint32_t now, uint32_t until, bool neighbors)
{
    if (!slot || slot > 32 || (int32_t)(until - now) <= 0) return false;
    for (unsigned i = 0; i < 32; i++) colors[i] = neighbors ? 0 : i == 0 ? 0x002000 : 0;
    colors[slot - 1] = slot == 1 && !neighbors ? 0x002000 : 0x202020;
    if (neighbors) {
        colors[(slot + 30) % 32] = 0x200000;
        colors[slot % 32] = 0x200000;
    }
    return true;
}
#endif
