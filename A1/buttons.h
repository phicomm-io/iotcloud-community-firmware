#pragma once
#include "../common/gesture.h"

typedef struct {
    key_gesture_t keys[3];
    bool chord;
    unsigned released;
} a1_buttons_t;

/* Once both keys overlap, cancel singles until both are released. A staggered
 * release must never become a new single press or a Wi-Fi recovery gesture. */
static inline void a1_buttons_sample(a1_buttons_t *b, bool valid, bool power,
                                     bool wifi, uint32_t now, key_event_t out[3])
{
    memset(out, 0, 3 * sizeof(*out));
    if (!valid) {
        for (unsigned i = 0; i < 3; i++)
            key_gesture_sample(&b->keys[i], false, false, now, false);
        b->chord = false;
        b->released = 0;
        return;
    }
    if (power && wifi && !b->chord) {
        b->chord = true;
        b->released = 0;
        for (unsigned i = 0; i < 2; i++)
            key_gesture_sample(&b->keys[i], false, false, now, false);
    }
    if (b->chord) {
        out[2] = key_gesture_sample(&b->keys[2], true, power && wifi, now, false);
        if (!power && !wifi) b->released++;
        else b->released = 0;
        if (b->released >= 2) {
            b->chord = false;
            for (unsigned i = 0; i < 2; i++)
                key_gesture_sample(&b->keys[i], true, false, now, false);
        }
    } else {
        out[0] = key_gesture_sample(&b->keys[0], true, power, now, false);
        out[1] = key_gesture_sample(&b->keys[1], true, wifi, now, true);
        out[2] = key_gesture_sample(&b->keys[2], true, false, now, false);
    }
}
