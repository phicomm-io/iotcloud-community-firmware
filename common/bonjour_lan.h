/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool bonjour_lan_txt(char *,size_t,const char *id,const char *model,const char *variant,const char *firmware,uint16_t port);
