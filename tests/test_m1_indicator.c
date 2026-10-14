#include "../M1/indicator.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    for (unsigned bits = 0; bits < 16; bits++) {
        bool configured = bits & 8, wifi = bits & 4, mqtt_configured = bits & 2, mqtt = bits & 1;
        unsigned expected = !configured ? M1_LED_UNCONFIGURED :
            !wifi ? M1_LED_WIFI_DOWN : mqtt_configured && !mqtt ? M1_LED_MQTT_DOWN : M1_LED_READY;
        assert(m1_indicator_mode(configured, wifi, mqtt_configured, mqtt) == expected);
    }
    /* Check two complete cycles, including every transition boundary. */
    for (uint32_t ms = 0; ms < 7200; ms++) {
        assert(m1_indicator_on(M1_LED_WIFI_DOWN, ms) == (ms % 1000 < 500));
        assert(!m1_indicator_on(M1_LED_READY, ms));
        assert(m1_indicator_on(M1_LED_UNCONFIGURED, ms) == (ms % 1000 < 500));
        unsigned phase = ms % 3600;
        bool expected = false;
        const unsigned starts[] = {0, 1000, 2000, 2400};
        const unsigned lengths[] = {800, 800, 200, 200};
        for (unsigned pulse = 0; pulse < 4; pulse++)
            if (phase >= starts[pulse] && phase < starts[pulse] + lengths[pulse]) expected = true;
        assert(m1_indicator_on(M1_LED_MQTT_DOWN, ms) == expected);
    }
    /* Unsigned elapsed time remains valid across the RTOS tick rollover. */
    uint32_t since = UINT32_MAX - 99u, now = 400u;
    assert(!m1_indicator_on(M1_LED_UNCONFIGURED, (uint32_t)(now - since)));
    puts("M1 indicator state priority, pulse timing and tick rollover passed");
    return 0;
}
