#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { M1_LED_UNCONFIGURED, M1_LED_WIFI_DOWN, M1_LED_MQTT_DOWN, M1_LED_READY };

static inline unsigned m1_indicator_mode(bool configured, bool wifi, bool mqtt_configured, bool mqtt)
{
    if (!configured) return M1_LED_UNCONFIGURED;
    if (!wifi) return M1_LED_WIFI_DOWN;
    return !mqtt_configured || mqtt ? M1_LED_READY : M1_LED_MQTT_DOWN;
}

/* Elapsed time is relative to entry into the current mode. */
static inline bool m1_indicator_on(unsigned mode, uint32_t elapsed)
{
    if (mode == M1_LED_UNCONFIGURED || mode == M1_LED_WIFI_DOWN)
        return elapsed % 1000u < 500u;
    if (mode == M1_LED_MQTT_DOWN) {
        uint32_t phase = elapsed % 3600u;
        return phase < 800u || (phase >= 1000u && phase < 1800u) ||
               (phase >= 2000u && phase < 2200u) ||
               (phase >= 2400u && phase < 2600u);
    }
    return false;
}
