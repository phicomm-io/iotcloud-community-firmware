/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The lab codec is adapted to reject JSON at all public entry points. Accept
 * only MessagePack/CBOR; JSON is used exclusively by HA's separate emitter. */
#define PH_WIRE_PRIMITIVES_EXTERNAL
#include "../protocol/typed_wire.h"
#define PH_WIRE_PRIMITIVE
#include "../protocol/wire_primitives.inc"
#define PH_BINARY_PRIMITIVE
#include "../protocol/binary_reader.h"
#include "../protocol/binary_reader.inc"
#include "../protocol/typed_wire.inc"

/* Only device-owned maps from the fixed writer enter here. A MessagePack
 * reader copies the encoded fields without parsing or rebuilding the map;
 * CBOR readers use the same validated borrowed-value transcoder. */
void phw_cached_fields(ph_writer *w,const void *data,size_t size) {
 const unsigned char *p=data;
 if(!w->depth||w->arrays[w->depth-1]||size<3||p[0]!=0xde){w->failed=true;return;}
 unsigned count=((unsigned)p[1]<<8)|p[2];
 if(w->format==PH_WIRE_MSGPACK){
  unsigned d=w->depth-1;
  if((unsigned)w->counts[d]+count>UINT16_MAX){w->failed=true;return;}
  w->counts[d]+=count;phw_bytes(w,p+3,size-3);
 } else {
  ph_value root,value;ph_iterator it;char key[128];
  if(!phv_parse(PH_WIRE_MSGPACK,p,size,&root)||!phv_iter(&root,&it)){w->failed=true;return;}
  while(phv_next(&it,key,&value))phw_value(w,key,&value);
 }
}

/* Shared MQTT admission uses this before allocating a command queue entry. */
bool ph_binary_request_id(const void *bytes,size_t n,char id[65]) {
    ph_value root,v;id[0]=0;ph_wire_format f=ph_wire_detect(bytes,n);
    if((f!=PH_WIRE_MSGPACK&&f!=PH_WIRE_CBOR)||
       !phv_parse(f,bytes,n,&root)||root.kind!=PHV_MAP||
       !phv_get(&root,"request_id",&v)||!phv_text(&v,id,65))return false;
    for(const char *s=id;*s;++s)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.')){id[0]=0;return false;}
    return *id!=0;
}
#include <stdio.h>
int json_number_scan(const char *s, double *out) {
    char *end; double value = strtod(s, &end);
    if (end == s || *end || !isfinite(value)) return 0;
    *out = value; return 1;
}
int json_number_format(char *s, unsigned n, double v) {
    int k = snprintf(s, n, "%.17g", v); return k >= 0 && (unsigned)k < n ? k : -1;
}
int json_number_format_float32(char *s, unsigned n, float v) {
    int k = snprintf(s, n, "%.9g", (double)v); return k >= 0 && (unsigned)k < n ? k : -1;
}
