/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Initialize immutable, independently copied identity before the network owner.
 * The hostname must exactly match the validated factory identity. */
bool PH_BL_MdnsInit(const char *hostname,const char *id,const char *variant,const char *firmware);
/* Called outside the product lock. ready requires STA IP + UDP bind + no OTA.
 * SDK responder operations run only in tcpip_callback, with one pending call. */
void PH_BL_MdnsPoll(bool ready);
bool PH_BL_MdnsActive(void);
unsigned PH_BL_MdnsFailures(void);
/* Bounded TXT schema, also exercised without importing a second DNS parser. */
size_t PH_BL_MdnsTxt(unsigned index,char *output,size_t capacity);
