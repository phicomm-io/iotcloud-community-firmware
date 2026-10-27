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
        /* Finish an expired click group before a new press starts, including
         * when its deadline falls between two panel scans. The key is already
         * stably released here; this only flushes its pending click event. */
        if (wifi && !b->keys[1].stable && !b->keys[1].candidate &&
            b->keys[1].clicks && (uint32_t)(now - b->keys[1].released_at) >= 300u)
            out[1] = key_gesture_sample(&b->keys[1], true, false, now, true);
        unsigned wifi_clicks = b->keys[1].clicks;
        out[0] = key_gesture_sample(&b->keys[0], true, power, now, false);
        key_event_t wifi_event = key_gesture_sample(&b->keys[1], true, wifi, now, true);
        if (wifi_event.type != KEY_EVENT_NONE) out[1] = wifi_event;
        /* Some original panels report release after ~2-3 s of continuous
         * touch. Two/three short taps followed by a >=1 s hold provide an
         * intentional network/full recovery without inventing a held input.
         * Use only the still-pending click group and act on reported release;
         * expired groups, failed reads and chords cannot arm recovery. */
        if (out[1].type == KEY_EVENT_HOLD &&
            (wifi_clicks == 2 || wifi_clicks == 3)) {
            out[1].type = wifi_clicks == 2 ? KEY_EVENT_NETWORK_RESET : KEY_EVENT_FULL_RESET;
            out[1].count = 0;
        }
        out[2] = key_gesture_sample(&b->keys[2], true, false, now, false);
    }
}
