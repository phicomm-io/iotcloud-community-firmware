/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "screen_link.h"
#define SCREEN_CAP_WIDGETS 1u
#define SCREEN_CAP_FLASH_INFO 2u
#define SCREEN_CAP_SCENE 4u
#define SCREEN_CAP_SERVICES 8u
#define SCREEN_CAP_BACKLIGHT 16u
#define SCREEN_CAP_DISPLAY 32u
#define SCREEN_CAP_SCENE_V3 64u
#define SCREEN_CAP_VALUE_MODE 128u
#define SCREEN_CAP_RESPONSIVE 256u
#define SCREEN_CAP_APPEARANCE 512u
#define SCREEN_CAP_WALLPAPER 2048u
#define SCREEN_CAP_MULTI_UNITS 4096u
#define SCREEN_CAP_MODULE_CONTROL 1024u
#define SCREEN_CAP_HIDE_TITLE 8192u
#define SCREEN_CAP_FONT_BANK 16384u
#define SCREEN_CAP_LANGUAGE 32768u
#define SCREEN_CAP_CHART_POINTS 65536u
#define SCREEN_CAP_PAGE_RAIL 131072u
#define SCREEN_CAP_RESOURCE_IO 262144u
#define SCREEN_CAP_DISPLAY_DIAG 524288u
#define SCREEN_CAP_PAGE_CONTROL 1048576u
#define SCREEN_CAP_COMPACT_WIDGETS 2097152u
#define SCREEN_CAP_MEDIA 4194304u
#define SCREEN_CAP_IMAGE 8388608u
#define SCREEN_CAP_BINARY_LINK 16777216u
#define SCREEN_CAP_KNOWN (SCREEN_CAP_WIDGETS | SCREEN_CAP_FLASH_INFO | SCREEN_CAP_SCENE | SCREEN_CAP_SERVICES | SCREEN_CAP_BACKLIGHT | SCREEN_CAP_DISPLAY | SCREEN_CAP_SCENE_V3 | SCREEN_CAP_VALUE_MODE | SCREEN_CAP_RESPONSIVE | SCREEN_CAP_MODULE_CONTROL | SCREEN_CAP_APPEARANCE | SCREEN_CAP_WALLPAPER | SCREEN_CAP_MULTI_UNITS | SCREEN_CAP_HIDE_TITLE | SCREEN_CAP_FONT_BANK | SCREEN_CAP_LANGUAGE | SCREEN_CAP_CHART_POINTS | SCREEN_CAP_PAGE_RAIL | SCREEN_CAP_RESOURCE_IO | SCREEN_CAP_DISPLAY_DIAG | SCREEN_CAP_PAGE_CONTROL | SCREEN_CAP_COMPACT_WIDGETS | SCREEN_CAP_MEDIA | SCREEN_CAP_IMAGE | SCREEN_CAP_BINARY_LINK)
/* Screen sends one offer after the normal factory version exchange. Old ESP
 * firmware may reject it; no retries and no binary extension in fallback. */
typedef struct {uint32_t session,capabilities;bool ready;} screen_caps;
void screen_caps_reset(screen_caps *);
size_t screen_caps_offer(screen_caps *,char *,size_t,uint32_t,uint32_t);
bool screen_caps_parse_offer(const char *,size_t,uint32_t *,uint32_t *);
size_t screen_caps_ack(char *,size_t,uint32_t,uint32_t);
bool screen_caps_accept(screen_caps *,const char *,size_t);
