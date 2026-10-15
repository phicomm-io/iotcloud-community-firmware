#pragma once
#include "../common/indicator.h"
#define M1_LED_UNCONFIGURED INDICATOR_UNCONFIGURED
#define M1_LED_WIFI_DOWN INDICATOR_WIFI_DOWN
#define M1_LED_MQTT_DOWN INDICATOR_MQTT_DOWN
#define M1_LED_READY INDICATOR_READY
#define m1_indicator_mode indicator_mode
#define m1_indicator_on indicator_on
static inline bool m1_indicator_wifi(unsigned mode, uint32_t elapsed)
{
    return mode == M1_LED_READY || mode == M1_LED_MQTT_DOWN ||
           m1_indicator_on(mode, elapsed);
}
static inline bool m1_indicator_error(unsigned mode, uint32_t elapsed)
{
    return (mode == M1_LED_WIFI_DOWN || mode == M1_LED_MQTT_DOWN) &&
           m1_indicator_on(mode, elapsed);
}
