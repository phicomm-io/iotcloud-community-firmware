/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../wire_format.h"
#include <string.h>
/* Small, fixed-capacity MessagePack emitter for MQTT bootstrap/OTA metadata.
 * No object tree, allocator, float formatting or implicit string length on IO. */
typedef struct { unsigned char *bytes;size_t capacity,used;bool failed; } ph_mqtt_wire;
static inline void ph_mw_bytes(ph_mqtt_wire *w,const void *p,size_t n) {
    if(w->failed)return;
    if(n>w->capacity-w->used){w->failed=true;return;}
    memcpy(w->bytes+w->used,p,n);w->used+=n;
}
static inline void ph_mw_head(ph_mqtt_wire *w,unsigned kind,uint64_t n) {
    unsigned char b[9];size_t length=ph_wire_head(PH_WIRE_MSGPACK,kind,n,b);ph_mw_bytes(w,b,length);
}
static inline void ph_mw_text(ph_mqtt_wire *w,const char *s) {
    size_t n=strlen(s);ph_mw_head(w,3,n);ph_mw_bytes(w,s,n);
}
static inline void ph_mw_string(ph_mqtt_wire *w,const char *key,const char *s) {
    ph_mw_text(w,key);ph_mw_text(w,s);
}
static inline void ph_mw_uint(ph_mqtt_wire *w,const char *key,uint32_t n) {
    ph_mw_text(w,key);ph_mw_head(w,0,n);
}
static inline void ph_mw_bool(ph_mqtt_wire *w,const char *key,bool value) {
    ph_mw_text(w,key);unsigned char b=value?0xc3:0xc2;ph_mw_bytes(w,&b,1);
}
