/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* RFC 4330 unicast checks. Subtraction unfolds the 2036 NTP era rollover
 * into the supported Unix 2020..2038 interval. Invalid replies never set time. */
static inline bool ph_ntp_reply(const unsigned char *p,size_t n,const unsigned char nonce[8],uint32_t *utc){
    if(n<48||(p[0]>>6)==3||((p[0]>>3)&7)!=4||(p[0]&7)!=4||!p[1]||p[1]>15||memcmp(p+24,nonce,8))return false;
    unsigned nonzero=0;for(unsigned i=40;i<48;i++)nonzero|=p[i];if(!nonzero)return false;
    uint32_t seconds=((uint32_t)p[40]<<24)|((uint32_t)p[41]<<16)|((uint32_t)p[42]<<8)|p[43];
    uint32_t value=seconds-UINT32_C(2208988800);
    if(value<1577836800u||value>INT32_MAX)return false;*utc=value;return true;
}
void ph_ntp_poll(const char *,bool enabled);
