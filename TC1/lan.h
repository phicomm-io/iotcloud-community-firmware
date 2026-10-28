/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "mico.h"
/* Dedicated UDP owner; unlike A1/M1, TC1 had no binding worker to reuse. */
#define TC1_LAN_STACK_SIZE 12288u
OSStatus tc1_lan_start(void);
bool tc1_lan_pause(unsigned timeout_ms);
