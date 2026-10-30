/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "mico.h"
/* Dedicated UDP owner; unlike A1/M1, TC1 had no binding worker to reuse. */
/* Actual linked LAN paths plus the unchanged 2 KiB SDK/crypto reserve must
 * fit this bound (checked for every build). Frees 4 KiB for binary replies. */
#define TC1_LAN_STACK_SIZE 8192u
OSStatus tc1_lan_start(void);
bool tc1_lan_pause(unsigned timeout_ms);
