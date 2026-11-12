/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "wire_format.h"
#define SERVICE_BUSY_BYTES 160
extern bool ph_binary_request_id(const void *,size_t,char [65]) __attribute__((weak));
/* Binary-only admission response. JSON is reserved for HA discovery/state. */
static inline void service_busy_id(const void *payload,size_t length,char id[65]) {
 id[0]=0;
 if(ph_wire_detect(payload,length) && ph_binary_request_id)
  (void)ph_binary_request_id(payload,length,id);
}
