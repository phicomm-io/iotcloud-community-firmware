/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#ifndef IOTCLOUD_SCREEN_SCENE_H
#define IOTCLOUD_SCREEN_SCENE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SCENE_PAGES 8
#define SCENE_ITEMS 32
#define SCENE_POINTS 64
#define SCENE_BYTES (16 + 36 * SCENE_PAGES + 288 * SCENE_ITEMS)
#define SCENE_SERIES 3
#define SCENE_SERIES_POINTS 32
enum { SC_CH_LINE, SC_CH_MULTI, SC_CH_DUAL, SC_CH_BAR, SC_CH_STACK_BAR, SC_CH_STACK_AREA, SC_CH_RING };
enum { SC_VALUE_AUTO, SC_VALUE_NUMBER, SC_VALUE_GRADE };
enum {
  SC_CLOCK,
  SC_VALUE,
  SC_TEXT,
  SC_LINE,
  SC_BAR,
  SC_PIE,
  SC_WEATHER,
  SC_REMINDERS
};
enum {
  SC_BIND_TEMP,
  SC_BIND_HUM,
  SC_BIND_CO2,
  SC_BIND_PM25,
  SC_BIND_TVOC,
  SC_BIND_HCHO,
  SC_BIND_EXTERNAL = 6
};
enum {
  SC_MSG_BEGIN = 0x20,
  SC_MSG_CHUNK,
  SC_MSG_COMMIT,
  SC_MSG_DATA,
  SC_MSG_WEATHER,
  SC_MSG_REMINDER,
  SC_MSG_CLOCK,
  SC_MSG_SERIES = 0x29,
  SC_MSG_ACK = 0x2f
};
typedef struct {
  bool enabled;
  char title[32];
  uint32_t id;
} scene_page;
typedef struct {
  uint16_t id;
  uint8_t page, type, binding, x, y, w, h, source;
  char title[49], unit[17];
  uint8_t chart, hours, series_count, binding2, binding3, additive;
  char text[193];
  uint8_t value_mode, hide_title;
  uint16_t points; /* 0 selects 32 points; local charts allow 32/64/128/256. */
} scene_item;
typedef struct {
  uint32_t revision;
  uint8_t pages, count, schema;
  scene_page page[SCENE_PAGES];
  scene_item item[SCENE_ITEMS];
} screen_scene;
typedef struct {
  uint16_t id;
  uint8_t count;
  bool valid, expired;
  uint32_t at, ttl;
  char text[193];
  float values[SCENE_POINTS];
  uint8_t series_count;
  char names[SCENE_SERIES][32], units[SCENE_SERIES][16];
  uint32_t masks[SCENE_SERIES];
  float series[SCENE_SERIES][SCENE_SERIES_POINTS];
} scene_data;
typedef struct {
  uint8_t bytes[SCENE_BYTES];
  uint32_t revision, crc, received, total;
  bool active;
} scene_transaction;
void scene_defaults(screen_scene *);
bool scene_validate(const screen_scene *);
bool scene_requires_multi_units(const scene_item *);
bool scene_requires_compact(const scene_item *);
size_t scene_wire_size(const screen_scene *);
bool scene_encode(const screen_scene *, uint8_t out[SCENE_BYTES]);
bool scene_decode(screen_scene *, const uint8_t *, size_t);
/* Strict contiguous chunks, bounded total, atomic replacement on commit. */
bool scene_receive(scene_transaction *, screen_scene *, const uint8_t *,
                   size_t);
bool scene_data_receive(scene_data *, const uint8_t *, size_t, uint32_t);
bool scene_series_receive(scene_data *, const uint8_t *, size_t, uint32_t);
bool scene_data_fresh(const scene_data *, uint32_t);
bool scene_utf8(const char *, size_t);
uint32_t scene_crc32(const void *, size_t);
uint32_t scene_u32(const uint8_t *);
void scene_put32(uint8_t *, uint32_t);

#endif
