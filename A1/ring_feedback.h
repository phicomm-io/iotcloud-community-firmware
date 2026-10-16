#pragma once
#include <stdint.h>
#include <string.h>

/* Two LED pairs form the moving edge. Smoothstep is zero/full with zero slope
 * at both ends; the 16-pair travel reaches the last pair fully only at 100%. */
static inline unsigned a1_ring_level(unsigned speed, unsigned distance)
{
    if (!speed) return 0;
    if (!distance) return 255;
    if (speed > 100) speed = 100;
    int edge = (int)(16 * speed) - (int)(100 * (distance - 1));
    if (edge <= 0) return 0;
    if (edge >= 200) return 255;
    unsigned x = (unsigned)edge;
    return (255u * x * x * (600u - 2u * x) + 4000000u) / 8000000u;
}

static inline void a1_ring_feedback(uint32_t out[32], unsigned speed,
                                    unsigned brightness, uint32_t color)
{
    memset(out, 0, 32 * sizeof(*out));
    for (unsigned led = 1; led <= 31; led++) {
        unsigned distance = led > 10 ? led - 10 : 10 - led;
        if (distance > 15) distance = 31 - distance;
        unsigned level = a1_ring_level(speed, distance);
        unsigned r = ((color >> 8) & 255) * brightness / 255 * level / 255;
        unsigned g = ((color >> 16) & 255) * brightness / 255 * level / 255;
        unsigned b = (color & 255) * brightness / 255 * level / 255;
        /* Slot 27 is physical LED 10; slot 0 is outside the 31-source ring. */
        out[(led + 16) % 31 + 1] = (g << 16) | (r << 8) | b;
    }
}
