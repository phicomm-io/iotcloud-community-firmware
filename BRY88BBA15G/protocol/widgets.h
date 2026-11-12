#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SCREEN_WIDGETS 2
#define SCREEN_WIDGET_POINTS 16
#define SCREEN_WIDGET_TEXT 97
/* MessagePack array: [1, slot, type, source, ttl_seconds, title, unit, text,
 * values]. Types: value=0, text=1, line=2, bar=3, pie=4.
 * Source: API=0, MQTT=1, authorized LAN=2. Authorization belongs to ESP32. */
typedef struct {
 bool valid,expired;uint8_t type,source,count;uint32_t at,ttl_ms;
 char title[49],unit[17],text[SCREEN_WIDGET_TEXT];
 float values[SCREEN_WIDGET_POINTS];
} screen_widget;
typedef struct {screen_widget items[SCREEN_WIDGETS];} screen_widgets;
bool screen_widget_apply(screen_widgets *,const void *,size_t,uint32_t);
bool screen_widget_valid(const screen_widget *,uint32_t);
