/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { bool red, blue; } ph_indicator;
static inline ph_indicator ph_indicator_state(uint32_t now, bool enabled,
        bool relay, bool wifi_configured, bool wifi_connected,
        bool mqtt_configured, bool mqtt_ready, bool button_down, uint32_t held) {
    ph_indicator state = {false, false};
    /* Match TC1 key_hold_feedback: one 160 ms pulse per complete second,
     * measured from this press rather than the global uptime. Recovery
     * feedback remains visible even when normal indicators are off. */
    if (button_down) {
        bool pulse = held >= 1000u && held % 1000u < 160u;
        state.blue = pulse && (held < 5000u || held >= 10000u);
        state.red = pulse && held >= 5000u;
        return state;
    }
    if (!enabled) return state;
    if (!wifi_configured && !wifi_connected) {
        state.red = (now / 500u) & 1u;
        state.blue = !state.red;
    } else {
        state.blue = relay;
        if (!wifi_connected) state.red = (now / 150u) & 1u;
        else if (mqtt_configured && !mqtt_ready) state.red = (now / 1000u) & 1u;
    }
    return state;
}
