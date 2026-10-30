/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { bool red, blue; } ph_indicator;
static inline ph_indicator ph_indicator_state(uint32_t now, bool enabled,
        bool relay, bool wifi_configured, bool wifi_connected,
        bool mqtt_configured, bool mqtt_ready, bool button_down, uint32_t held) {
    ph_indicator state = {false, false};
    /* Recovery feedback remains visible even when normal indicators are off. */
    if (button_down && held >= 5000) {
        bool factory = held >= 10000;
        state.blue = (now / (factory ? 100u : 250u)) & 1u;
        state.red = factory && state.blue;
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
