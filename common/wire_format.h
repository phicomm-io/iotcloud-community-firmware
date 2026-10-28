/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
/* MessagePack subset with explicit binary values and string map keys. */
typedef enum { PH_WIRE_MSGPACK = 1 } ph_wire_format;
static inline const char *ph_wire_mime(ph_wire_format f)
{ return f==PH_WIRE_MSGPACK?"application/msgpack":NULL; }
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
