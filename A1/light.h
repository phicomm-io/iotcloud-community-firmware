#ifndef A1_LIGHT_H
#define A1_LIGHT_H
#include <stdint.h>
#include <stdbool.h>

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

/* DUT calibration: one-based 11 is bottom centre; 10/12 are symmetric.
 * Slots 26/27 flank the top axis. Tests found 31 independent positions;
 * slot 32 overlapped slot 1, so do not drive it as another physical LED.
 * Keep 32-element buffers for the shared renderer; the spare is always off.
 */
#define A1_RING_POSITIONS 31u
static inline void a1_fan_frame(uint32_t colors[32], unsigned speed, uint32_t color)
{
    if (speed > 100) speed = 100;
    unsigned units = (speed * 62 + 50) / 100;
    uint32_t half = (color & 0xfefefeu) >> 1;
    for (unsigned i = 0; i < 32; i++) colors[i] = 0;
    colors[10] = units >= 2 ? color : units ? half : 0;
    unsigned remaining = units > 2 ? units - 2 : 0;
    for (unsigned distance = 1; distance <= 15; distance++) {
        unsigned consumed = (distance - 1) * 4;
        uint32_t level = remaining >= consumed + 4 ? color :
            remaining >= consumed + 2 ? half : 0;
        colors[(10 + A1_RING_POSITIONS - distance) % A1_RING_POSITIONS] = level;
        colors[(10 + distance) % A1_RING_POSITIONS] = level;
    }
}
#endif
