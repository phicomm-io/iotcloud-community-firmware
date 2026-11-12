/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { PH_BUTTON_NONE, PH_BUTTON_CLICK, PH_BUTTON_WIFI, PH_BUTTON_RESET } ph_button_event;
typedef struct { bool raw, down; uint32_t changed, pressed; } ph_button;
/* Debounce and classify only on release: a 10 s hold must not fire at 5 s. */
static inline ph_button_event ph_button_tick(ph_button *b, bool down, uint32_t t) {
    if (down != b->raw) { b->raw = down; b->changed = t; }
    if (down == b->down || (uint32_t)(t-b->changed) < 40) return PH_BUTTON_NONE;
    b->down = down;
    if (down) { b->pressed = b->changed; return PH_BUTTON_NONE; }
    uint32_t held = b->changed - b->pressed;
    return held >= 10000 ? PH_BUTTON_RESET : held >= 5000 ? PH_BUTTON_WIFI : held < 1000 ? PH_BUTTON_CLICK : PH_BUTTON_NONE;
}
