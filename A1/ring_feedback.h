#pragma once
#include <stdint.h>
#include "light.h"

/* The touch/fan overlay is the calibrated reference. Normal speed mode must
 * use the same origin, symmetric growth and half-brightness edge. */
static inline void a1_ring_feedback(uint32_t out[32], unsigned speed,
                                    unsigned brightness, uint32_t color)
{
    unsigned g = ((color >> 16) & 255) * brightness / 255;
    unsigned r = ((color >> 8) & 255) * brightness / 255;
    unsigned b = (color & 255) * brightness / 255;
    a1_fan_frame(out, speed, (g << 16) | (r << 8) | b);
}
