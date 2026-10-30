/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool PH_BL_Sha256(const void *,size_t,uint8_t[32]);
