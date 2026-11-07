#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define APPEARANCE_BYTES 704u
#define SCREEN_MSG_APPEARANCE 0x30u
#define APPEARANCE_ITEMS 32u
enum { AP_CANVAS, AP_BACKGROUND, AP_TITLE, AP_VALUE, AP_UNIT, AP_BODY,
       AP_ACCENT, AP_LINE, AP_CHART_A, AP_CHART_B, AP_CHART_C, AP_DANGER,
       AP_ROLES };
/* Widget wire roles: background, title, value, unit, body. 255 inherits alpha. */
typedef struct { uint16_t id; uint8_t mask, opacity; uint32_t colors[5]; } appearance_item;
typedef struct {
  uint32_t revision, custom_crc;
  uint16_t mask;
  uint8_t theme, wallpaper, opacity, dim, count, language, page_rail;
  uint32_t colors[AP_ROLES];
  appearance_item items[APPEARANCE_ITEMS];
} screen_appearance;
void appearance_default(screen_appearance *);
unsigned appearance_rail_height(const screen_appearance *);
bool appearance_encode(const screen_appearance *, uint8_t *, size_t);
bool appearance_decode(screen_appearance *, const uint8_t *, size_t);
uint32_t appearance_color(const screen_appearance *, unsigned);
const appearance_item *appearance_find(const screen_appearance *, unsigned);
uint32_t appearance_widget_color(const screen_appearance *, const appearance_item *, unsigned);
unsigned appearance_opacity(const screen_appearance *, const appearance_item *, unsigned type, unsigned height, bool wallpaper);

#ifdef __cplusplus
}
#endif
