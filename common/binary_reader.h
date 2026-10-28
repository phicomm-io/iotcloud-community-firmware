/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "wire_format.h"
#include <limits.h>
#define PH_BINARY_DEPTH 12u
#define PH_BINARY_NODES 1024u
#define PH_BINARY_BYTES 8192u
#define PH_BINARY_SAFE UINT64_C(9007199254740991)
typedef struct {const unsigned char *p,*end;ph_wire_format f;unsigned nodes;bool failed;} binary_reader;
typedef struct {unsigned kind;union {uint64_t n;double real;};const unsigned char *text;} binary_item;
static inline bool binary_take(binary_reader *r,unsigned bytes,uint64_t *out)
{
    if(bytes>(size_t)(r->end-r->p))return false;
    *out=0;while(bytes--)*out=(*out<<8)|*r->p++;return true;
}
/* Standalone consumers use the header-inline token decoder. */
#ifdef PH_WIRE_PRIMITIVES_EXTERNAL
bool binary_token(binary_reader *,binary_item *);
#else
#define PH_BINARY_PRIMITIVE static inline
#include "binary_reader.inc"
#undef PH_BINARY_PRIMITIVE
#endif
