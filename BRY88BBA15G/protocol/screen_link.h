/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SCREEN_LINK_LINE 1536u
#define SCREEN_LINK_CHUNK 768u
#define SCREEN_LINK_SCHEMA 1u
/* Frames are ASCII CRLF lines, never raw binary. Parsed payload expires when
 * callback returns. Factory OTA binary mode must bypass this line parser. */
typedef void (*screen_link_receive)(void *,const char *,size_t);
typedef struct {char line[SCREEN_LINK_LINE];size_t used;bool overflow;screen_link_receive receive;void *context;} screen_link;
void screen_link_init(screen_link *,screen_link_receive,void *);
void screen_link_feed(screen_link *,const void *,size_t);
uint32_t screen_link_crc32(const void *,size_t);
size_t screen_link_encode(char *,size_t,uint32_t,uint32_t,const void *,size_t);
bool screen_link_decode(const char *,size_t,uint32_t *,uint32_t *,void *,size_t *);
