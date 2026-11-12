/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "scene.h"
#define SCREEN_MSG_POLICY 0x28u
#define DISPLAY_PLANS 8u
#define DISPLAY_KEEP_PAGE 255u
#define DISPLAY_POLICY_BYTES (16u + DISPLAY_PLANS * 44u)
typedef struct {
  bool enabled;
  uint8_t days, brightness, page; /* Mon=bit0; zero brightness switches off. */
  uint16_t start, end;
  char name[32];
} display_plan;
typedef struct {
  uint32_t revision;
  uint16_t external_minutes, battery_minutes;
  uint8_t ceiling, idle_brightness, idle_page, count;
  display_plan plans[DISPLAY_PLANS];
} display_policy;
#ifdef __cplusplus
extern "C" {
#endif
bool display_policy_valid(const display_policy *);
void display_policy_default(display_policy *);
bool display_policy_encode(const display_policy *, uint8_t *, size_t);
bool display_policy_decode(display_policy *, const uint8_t *, size_t);

#ifdef __cplusplus
}
#endif
