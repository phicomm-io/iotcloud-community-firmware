/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "protocol/screen_link.h"
#include "protocol/screen_caps.h"
/* This library does not configure GPIOs or acknowledge factory network commands.
 * Unhandled factory lines are delivered intact to the eventual command owner. */
typedef struct {
 screen_link parser;uint32_t session,last_sequence,tx_sequence;
 bool extended;uint32_t capabilities;
 /* send must enqueue the complete line atomically, returning false on failure. */
 bool (*send)(void *,const char *,size_t);
 void *context;
 void (*factory)(void *,const char *,size_t);
 void (*extension)(void *,const void *,size_t);
} bry_bridge;
void bry_bridge_init(bry_bridge *);
void bry_bridge_reset(bry_bridge *);
/* Caller invokes only after bidirectional capability/ACK handshake completes. */
bool bry_bridge_negotiate(bry_bridge *,unsigned,uint32_t);
void bry_bridge_feed(bry_bridge *,const void *,size_t);

/* Validates the widget before queuing one frame; returns false in stock mode. */
bool bry_bridge_publish(bry_bridge *,const void *,size_t);

bool bry_bridge_send(bry_bridge *,const void *,size_t,uint32_t capability);
