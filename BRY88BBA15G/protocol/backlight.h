/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MCU -> ESP, only after SCREEN_CAP_BACKLIGHT was acknowledged. Settings still
 * use the stock display.luminance command. Percentages are PWM duty, not nits. */
#define SCREEN_MSG_BACKLIGHT 0x27u
#define SCREEN_BACKLIGHT_BYTES 6u
typedef struct {
  uint8_t requested, ceiling, applied;
  bool asleep;
  uint8_t target, reason, page;
  uint32_t policy_revision;
} screen_backlight;

static inline bool screen_backlight_valid(unsigned requested, unsigned ceiling,
                                          unsigned applied, bool asleep) {
  return requested <= 100 && ceiling <= 100 && applied <= requested &&
         applied <= ceiling && (!asleep || !applied);
}
static inline bool screen_backlight_encode(uint8_t *out, unsigned requested,
                                           unsigned ceiling, unsigned applied,
                                           bool asleep) {
  if (!out || !screen_backlight_valid(requested, ceiling, applied, asleep))
    return false;
  out[0] = SCREEN_MSG_BACKLIGHT;
  out[1] = 1;
  out[2] = (uint8_t)requested;
  out[3] = (uint8_t)ceiling;
  out[4] = (uint8_t)applied;
  out[5] = asleep ? 1 : 0;
  return true;
}
static inline bool screen_backlight_decode(screen_backlight *out,
                                           const uint8_t *p, size_t n) {
  if (!out || !p) return false;
  if (n == 13 && p[0] == SCREEN_MSG_BACKLIGHT && p[1] == 2) {
    if (p[2] > 100 || p[3] > 100 || p[4] > p[3] || p[5] > 1 ||
        p[6] > 100 || p[4] > p[6] || p[7] > 2 || p[8] >= 8 ||
        (p[5] != (p[7] != 0))) return false;
    out->requested = p[2]; out->ceiling = p[3]; out->applied = p[4];
    out->asleep = p[5]; out->target = p[6]; out->reason = p[7]; out->page = p[8];
    out->policy_revision = (uint32_t)p[9] | (uint32_t)p[10] << 8 |
                           (uint32_t)p[11] << 16 | (uint32_t)p[12] << 24;
    return out->policy_revision != 0;
  }
  if (n != SCREEN_BACKLIGHT_BYTES || p[0] != SCREEN_MSG_BACKLIGHT || p[1] != 1 || p[5] > 1 ||
      !screen_backlight_valid(p[2], p[3], p[4], p[5] != 0))
    return false;
  out->requested = p[2];
  out->ceiling = p[3];
  out->applied = p[4];
  out->asleep = p[5] != 0;
  out->target = out->asleep ? 0 : out->requested;
  out->reason = out->asleep ? 1 : 0; out->page = 0; out->policy_revision = 0;
  return true;
}
