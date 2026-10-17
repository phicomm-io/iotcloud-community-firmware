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

/* Array index 11 (one-based LED 12) is the start; LEDs 11/13 are symmetric.
 * Slots 27/28 flank the opposite axis. Tests found 31 independent positions;
 * slot 32 overlapped slot 1, so do not drive it as another physical LED.
 * Keep 32-element buffers for the shared renderer; the spare is always off.
 */
#define A1_RING_POSITIONS 31u
#define A1_RING_ORIGIN 11u
typedef struct {
    uint32_t started, revision;
    bool initialized, automatic, active;
} a1_mode_light_t;

/* Packed GGRRBB, shared by mode confirmation and the current airflow bar. */
static inline uint32_t a1_mode_color(bool automatic)
{
    return automatic ? 0xa000ffu : 0x6eff00u;
}

/* Auto: two cyan pulses. Manual: one warm amber pulse. Observe both the
 * committed mode and explicit selections (including selecting it again).
 * Booting into a saved mode is silent. All state belongs to the LED worker.
 */
static inline bool a1_mode_light_frame(a1_mode_light_t *feedback,
    uint32_t revision, bool automatic, uint32_t now, unsigned brightness,
    uint32_t colors[32])
{
    bool changed = feedback->initialized
        ? feedback->revision != revision || feedback->automatic != automatic
        : revision != 0;
    feedback->initialized = true;
    feedback->revision = revision;
    feedback->automatic = automatic;
    if (changed) { feedback->started = now; feedback->active = true; }
    if (!feedback->active) return false;
    uint32_t elapsed = now - feedback->started;
    if (elapsed >= 1200) { feedback->active = false; return false; }
    unsigned phase = automatic ? elapsed % 600 : elapsed;
    unsigned width = automatic ? 450 : 1200;
    unsigned level = phase >= width ? 0 :
        (phase <= width / 2 ? phase : width - phase) * 255 / (width / 2);
    level = level * brightness / 255;
    uint32_t base = a1_mode_color(automatic);
    unsigned g = (base >> 16) & 255, r = (base >> 8) & 255;
    unsigned b = base & 255;
    uint32_t color = ((g * level / 255) << 16) |
        ((r * level / 255) << 8) | (b * level / 255);
    for (unsigned i = 0; i < A1_RING_POSITIONS; i++) colors[i] = color;
    colors[31] = 0;
    return true;
}

static inline void a1_fan_frame(uint32_t colors[32], unsigned speed, uint32_t color)
{
    if (speed > 100) speed = 100;
    unsigned units = (speed * 62 + 50) / 100;
    uint32_t half = (color & 0xfefefeu) >> 1;
    for (unsigned i = 0; i < 32; i++) colors[i] = 0;
    colors[A1_RING_ORIGIN] = units >= 2 ? color : units ? half : 0;
    unsigned remaining = units > 2 ? units - 2 : 0;
    for (unsigned distance = 1; distance <= 15; distance++) {
        unsigned consumed = (distance - 1) * 4;
        uint32_t level = remaining >= consumed + 4 ? color :
            remaining >= consumed + 2 ? half : 0;
        colors[(A1_RING_ORIGIN + A1_RING_POSITIONS - distance) % A1_RING_POSITIONS] = level;
        colors[(A1_RING_ORIGIN + distance) % A1_RING_POSITIONS] = level;
    }
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
