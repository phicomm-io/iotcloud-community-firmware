/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
/* JSON-compatible, definite-length subset. No extensions, tags, binary strings,
 * indefinite containers or non-finite numbers. Transport supplies the length. */
typedef enum { PH_WIRE_JSON, PH_WIRE_MSGPACK, PH_WIRE_CBOR } ph_wire_format;
static inline const char *ph_wire_mime(ph_wire_format f)
{ return f==PH_WIRE_MSGPACK?"application/msgpack":f==PH_WIRE_CBOR?"application/cbor":"application/json"; }
static inline ph_wire_format ph_wire_detect(const void *data,size_t n)
{
    if(!n || !data)return PH_WIRE_JSON;
    unsigned c=*(const unsigned char *)data;
    if((c>=0x80 && c<=0x8f) || c==0xde || c==0xdf)return PH_WIRE_MSGPACK;
    if(c>=0xa0 && c<=0xbb)return PH_WIRE_CBOR;
    return PH_WIRE_JSON;
}
/* Header writer is allocation free, also usable in a counting pass. */
#ifdef PH_WIRE_PRIMITIVES_EXTERNAL
size_t ph_wire_head(ph_wire_format,unsigned,uint64_t,unsigned char [9]);
size_t ph_wire_double(ph_wire_format,double,unsigned char [9]);
bool ph_wire_utf8(const unsigned char *,size_t);
#else
#define PH_WIRE_PRIMITIVE static inline
#include "wire_primitives.inc"
#undef PH_WIRE_PRIMITIVE
#endif
