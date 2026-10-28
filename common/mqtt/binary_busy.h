/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../typed_wire.h"
#include <string.h>
#define SERVICE_BUSY_BYTES 160u
static inline bool mqtt_busy_identifier(const char *id)
{
    if(!*id)return false;
    for(;*id;id++)if(!((*id>='a'&&*id<='z')||(*id>='A'&&*id<='Z')||(*id>='0'&&*id<='9')||*id=='.'||*id=='_'||*id=='-'))return false;
    return true;
}
/* Full core validation, without allocation, rejects duplicate keys/truncation. */
static inline void mqtt_busy_id(const void *data,size_t n,char id[65])
{
    ph_value root,value;id[0]=0;
    if(!phv_parse(PH_WIRE_MSGPACK,data,n,&root)||root.kind!=PHV_MAP||!phv_get(&root,"request_id",&value)||!phv_text(&value,id,65)||!mqtt_busy_identifier(id))id[0]=0;
}
static inline size_t mqtt_busy_format(const char *id,char out[SERVICE_BUSY_BYTES])
{
    unsigned char *p=(unsigned char *)out;size_t n=strlen(id);
    *p++=n?0x82:0x81;
    if(n){*p++=0xaa;memcpy(p,"request_id",10);p+=10;if(n<32)*p++=(unsigned char)(0xa0|n);else{*p++=0xd9;*p++=(unsigned char)n;}memcpy(p,id,n);p+=n;}
    *p++=0xa4;memcpy(p,"code",4);p+=4;*p++=0xa4;memcpy(p,"busy",4);p+=4;
    return (size_t)(p-(unsigned char *)out);
}
static inline size_t mqtt_busy_wire(const void *data,size_t n,char out[SERVICE_BUSY_BYTES])
{char id[65];mqtt_busy_id(data,n,id);return mqtt_busy_format(id,out);}
