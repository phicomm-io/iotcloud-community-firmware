/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* HA discovery/state only. One caller-owned buffer; no heap, parser, object
 * graph or printf dependency. Failure is sticky and must prevent publication. */
typedef struct {
    char *data;
    size_t length, capacity;
    unsigned depth;
    uint8_t kind[12], count[12];
    bool valid;
} ha_json;
/* Firmware shares this stateless codec across MQTT owners. Header-only mode
 * remains available to standalone consumers; each writer owns its own state. */
#if defined(PHICOMM_SHARED_HA_CODEC) && !defined(PHICOMM_HA_CODEC_IMPLEMENTATION)
void ha_json_init(ha_json *w,char *data,size_t capacity);
void haj_bytes(ha_json *w,const char *s,size_t n);
void haj_quote(ha_json *w,const char *s);
void haj_prefix(ha_json *w,const char *key);
void ha_json_begin(ha_json *w,const char *key,bool array);
void ha_json_end(ha_json *w);
void ha_json_string(ha_json *w,const char *key,const char *value);
void ha_json_bool(ha_json *w,const char *key,bool value);
void ha_json_int(ha_json *w,const char *key,int64_t value);
bool ha_json_done(const ha_json *w);
#else
#ifdef PHICOMM_SHARED_HA_CODEC
#define PH_HA_JSON_API
#else
#define PH_HA_JSON_API static inline
#endif
PH_HA_JSON_API void ha_json_init(ha_json *w,char *data,size_t capacity)
{ memset(w,0,sizeof(*w));w->data=data;w->capacity=capacity;w->valid=data && capacity;if(w->valid)data[0]=0; }
PH_HA_JSON_API void haj_bytes(ha_json *w,const char *s,size_t n)
{ if(!w->valid)return;if(n>=w->capacity-w->length){w->valid=false;return;}memcpy(w->data+w->length,s,n);w->length+=n;w->data[w->length]=0; }
PH_HA_JSON_API void haj_quote(ha_json *w,const char *s)
{
    static const char hex[]="0123456789abcdef";
    haj_bytes(w,"\"",1);
    for(;s && *s;s++) {
        unsigned char c=(unsigned char)*s;
        if(c=='"' || c=='\\'){char q[2]={'\\',(char)c};haj_bytes(w,q,2);}
        else if(c<32){char q[6]={'\\','u','0','0',hex[c>>4],hex[c&15]};haj_bytes(w,q,6);}
        else haj_bytes(w,s,1);
    }
    haj_bytes(w,"\"",1);
}
PH_HA_JSON_API void haj_prefix(ha_json *w,const char *key)
{
    if(!w->valid)return;
    if(!w->depth){if(w->length || key)w->valid=false;return;}
    unsigned d=w->depth-1;
    if((w->kind[d]=='{') != (key!=NULL)){w->valid=false;return;}
    if(w->count[d])haj_bytes(w,",",1);
    w->count[d]=1;
    if(key){haj_quote(w,key);haj_bytes(w,":",1);}
}
PH_HA_JSON_API void ha_json_begin(ha_json *w,const char *key,bool array)
{ haj_prefix(w,key);if(!w->valid)return;if(w->depth==12){w->valid=false;return;}char c=array?'[':'{';haj_bytes(w,&c,1);w->kind[w->depth]=c;w->count[w->depth++]=0; }
PH_HA_JSON_API void ha_json_end(ha_json *w)
{ if(!w->depth){w->valid=false;return;}char c=w->kind[--w->depth]=='{'?'}':']';haj_bytes(w,&c,1); }
PH_HA_JSON_API void ha_json_string(ha_json *w,const char *key,const char *value)
{ haj_prefix(w,key);haj_quote(w,value?value:""); }
PH_HA_JSON_API void ha_json_bool(ha_json *w,const char *key,bool value)
{ haj_prefix(w,key);haj_bytes(w,value?"true":"false",value?4:5); }
PH_HA_JSON_API void ha_json_int(ha_json *w,const char *key,int64_t value)
{
    char digits[21];size_t n=0;uint64_t u=value<0?0-(uint64_t)value:(uint64_t)value;
    do{digits[n++]=(char)('0'+u%10);u/=10;}while(u);
    haj_prefix(w,key);if(value<0)haj_bytes(w,"-",1);
    while(n)haj_bytes(w,&digits[--n],1);
}
PH_HA_JSON_API bool ha_json_done(const ha_json *w)
{ return w->valid && !w->depth && w->length; }

#undef PH_HA_JSON_API
#endif
