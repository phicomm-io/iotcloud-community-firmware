/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SCREEN_MSG_MODULE_REQUEST 0x2bu
#define SCREEN_MSG_MODULE_STATUS 0x2cu
#define SCREEN_MODULE_REQUEST_BYTES 12u
#define SCREEN_MODULE_STATUS_BYTES 20u
#define SCREEN_MODULE_MAX_MS 60000u
/* v1 request: opcode, version, action (query/high/leased low), reserved,
 * request ID u32 LE, low duration u32 LE. Query/high require duration zero.
 * Status: opcode, version, result (ok/invalid/unavailable), flags
 * (initialized/pin-high/auto-restore), request ID, remaining ms,
 * valid UART2 frames, output transitions; all integers u32 LE.
 * Request ID zero is reserved for unsolicited status. No state is persisted. */
enum { SCREEN_MODULE_QUERY, SCREEN_MODULE_HIGH, SCREEN_MODULE_LOW };
typedef struct {
  void *context;
  void (*write)(void *, bool);
  bool (*read)(void *);
  uint32_t started, duration, frames, transitions;
  bool initialized, inhibited;
} screen_module;
void screen_module_init(screen_module *, void *, void (*)(void *, bool),
                        bool (*)(void *));
void screen_module_restore(screen_module *);
/* Application-owned startup/OTA interlock; leases and link reset cannot lift it. */
void screen_module_inhibit(screen_module *, bool);
bool screen_module_tick(screen_module *, uint32_t);
bool screen_module_request(screen_module *, const uint8_t *, size_t,
                           uint32_t, uint8_t[SCREEN_MODULE_STATUS_BYTES]);
void screen_module_status(screen_module *, uint32_t, uint32_t, unsigned,
                          uint8_t[SCREEN_MODULE_STATUS_BYTES]);
