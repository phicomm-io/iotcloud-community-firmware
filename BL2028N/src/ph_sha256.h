/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool PH_BL_Sha256(const void *,size_t,uint8_t[32]);
/* One native OTA owns this stream. A NULL digest keeps manual URL upgrades. */
bool PH_BL_OTAHashSet(const char *,uint32_t);
bool PH_BL_OTAHashUpdate(const void *,size_t,uint32_t);
bool PH_BL_OTAHashVerify(void);
