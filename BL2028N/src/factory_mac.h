/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PH_S30_RF_ADDRESS UINT32_C(0x1e0000)
#define PH_S30_RF_SIZE 4096u
static inline uint32_t ph_le32(const unsigned char *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static inline bool ph_mac_valid(const unsigned char mac[6]) {
    unsigned any=0,not_ff=0,suffix=0;
    for(unsigned i=0;i<6;++i){any|=mac[i];not_ff|=mac[i]^255u;if(i>=3)suffix|=mac[i];}
    return !(mac[0]&1u)&&any&&not_ff&&suffix;
}
static inline bool ph_mac_ap_name(char out[24],const unsigned char mac[6]) {
    out[0]=0;if(!ph_mac_valid(mac))return false;
    snprintf(out,24,"S30M_V1B_%02X%02X%02X",mac[3],mac[4],mac[5]);return true;
}
static inline bool ph_mac_host_name(char out[32],const unsigned char mac[6]) {
    out[0]=0;if(!ph_mac_valid(mac))return false;
    snprintf(out,32,"iotcloud-s30-v1b-%02x%02x%02x",mac[3],mac[4],mac[5]);return true;
}
/* Beken calibration TLVs: header TLV\0, nested TXID, TXID_MAC(6).
 * Bound every nested item to its parent. Never scan code bytes for a MAC. */
static inline bool ph_factory_mac(const unsigned char *sector,size_t size,unsigned char out[6]) {
    if(size<8||ph_le32(sector)!=UINT32_C(0x00564c54))return false;
    uint32_t length=ph_le32(sector+4);if(length>size-8)return false;
    size_t end=8+length;
    for(size_t at=8;at<end;) {
        if(end-at<8)return false;uint32_t type=ph_le32(sector+at),n=ph_le32(sector+at+4);at+=8;
        if(n>end-at)return false;
        if(type==UINT32_C(0x11111100)) {
            size_t child_end=at+n;
            for(size_t child=at;child<child_end;) {
                if(child_end-child<8)return false;
                uint32_t item=ph_le32(sector+child),bytes=ph_le32(sector+child+4);child+=8;
                if(bytes>child_end-child)return false;
                if(item==UINT32_C(0x11111102)) {
                    if(bytes!=6||!ph_mac_valid(sector+child))return false;
                    memcpy(out,sector+child,6);return true;
                }
                child+=bytes;
            }
        }
        at+=n;
    }
    return false;
}
