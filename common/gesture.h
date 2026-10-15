#pragma once
#include <stdint.h>
enum { GESTURE_ORDINARY, GESTURE_NETWORK_RESET, GESTURE_FULL_RESET };
/* Evaluate on release only. Unsigned elapsed ticks survive RTOS wraparound. */
static inline unsigned gesture_release(uint32_t pressed_at, uint32_t released_at)
{
    uint32_t elapsed = released_at - pressed_at;
    return elapsed >= 10000u ? GESTURE_FULL_RESET :
           elapsed >= 5000u ? GESTURE_NETWORK_RESET : GESTURE_ORDINARY;
}
