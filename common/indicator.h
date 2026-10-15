#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Shared priority and timing, independent of product output hardware. */
enum { INDICATOR_UNCONFIGURED, INDICATOR_WIFI_DOWN,
       INDICATOR_MQTT_DOWN, INDICATOR_READY };
static inline unsigned indicator_mode(bool configured, bool wifi,
                                      bool mqtt_configured, bool mqtt_ready)
{
    if (!configured) return INDICATOR_UNCONFIGURED;
    if (!wifi) return INDICATOR_WIFI_DOWN;
    return mqtt_configured && !mqtt_ready ? INDICATOR_MQTT_DOWN : INDICATOR_READY;
}
static inline bool indicator_on(unsigned mode, uint32_t elapsed)
{
    if (mode == INDICATOR_WIFI_DOWN) return elapsed % 300u < 150u;
    if (mode == INDICATOR_MQTT_DOWN) return elapsed % 2000u < 1000u;
    if (mode == INDICATOR_UNCONFIGURED) {
        uint32_t phase = elapsed % 6000u;
        if (phase < 3000u) return phase % 1000u < 800u;
        if (phase < 4200u) return (phase - 3000u) % 400u < 200u;
    }
    return false;
}
