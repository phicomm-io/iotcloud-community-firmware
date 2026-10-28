/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Borrowed, validated MessagePack values; no parser allocations. */
#include "wire_format.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wire_buffer.h"
#define PHV_MISSING 255u
#define PHV_UINT 0u
#define PHV_NEG 1u
#define PHV_BYTES 2u
#define PHV_TEXT 3u
#define PHV_ARRAY 4u
#define PHV_MAP 5u
#define PHV_FALSE 20u
#define PHV_TRUE 21u
#define PHV_NULL 22u
#define PHV_REAL 27u
/* kind selects count (integers/containers) or real; integers retain all 53 safe bits. */
typedef struct { const unsigned char *begin,*end,*text; size_t size; union { uint64_t count; double real; }; ph_wire_format format; unsigned kind; } ph_value;
/* Parse validates a complete MessagePack document, duplicate string keys,
 * depth/nodes/UTF8/safe numeric range. Borrowed pointers expire with input. */
bool phv_parse(ph_wire_format,const void *,size_t,ph_value *);
bool phv_get(const ph_value *,const char *,ph_value *);
bool phv_at(const ph_value *,unsigned,ph_value *);
/* key/value at map index; decoded keys are <128 bytes. */
typedef struct { const unsigned char *cursor,*end; ph_wire_format format; unsigned kind,index,count; } ph_iterator;
bool phv_iter(const ph_value *,ph_iterator *);
bool phv_next(ph_iterator *,char key[128],ph_value *);
bool phv_pair(const ph_value *,unsigned,char key[128],ph_value *);
bool phv_bytes(const ph_value *,const void **,size_t *);
bool phv_text(const ph_value *,char *,size_t);
bool phv_equal_text(const ph_value *,const char *);
bool phv_int(const ph_value *,int64_t *);
bool phv_number(const ph_value *,double *);
bool phv_bool(const ph_value *,bool *);
bool phv_keys(const ph_value *,const char *allowed); /* |key||key| */
static inline bool phv_exists(const ph_value *v){return v && v->kind!=PHV_MISSING;}
/* Owned bounded output. Container lengths backpatched; key=NULL for root/array.
 * One writer per response, no mutation is executed twice for a sizing pass. */
#define PHW_DEPTH 12u
#define PHW_MAX_BYTES 16384u
typedef struct { ph_buf *wire; ph_wire_format format; size_t starts[PHW_DEPTH]; uint16_t counts[PHW_DEPTH]; bool arrays[PHW_DEPTH]; unsigned depth; bool failed,fixed; const char *root_key; } ph_writer;
bool phw_init(ph_writer *,ph_wire_format);
/* Capacity includes the trailing NUL; measure borrows only a buffer counter. */
bool phw_init_capacity(ph_writer *,ph_wire_format,size_t);
bool phw_init_measure(ph_writer *,ph_wire_format,ph_buf *);
bool phw_init_fixed(ph_writer *,ph_wire_format,ph_buf *,void *,size_t);
size_t phw_finish_fixed(ph_writer *); /* zero on failure, caller retains storage */
void phw_root_key(ph_writer *,const char *key); /* next bare root is a member of current object */
void phw_begin(ph_writer *,const char *key,bool array);
void phw_end(ph_writer *);
void phw_bytes_value(ph_writer *,const char *key,const void *,size_t);
void phw_text(ph_writer *,const char *key,const char *);
void phw_text_n(ph_writer *,const char *key,const char *,size_t);
void phw_int(ph_writer *,const char *key,int64_t);
void phw_uint(ph_writer *,const char *key,uint64_t);
void phw_i32(ph_writer *,const char *key,int32_t);
void phw_u32(ph_writer *,const char *key,uint32_t);
void phw_number(ph_writer *,const char *key,double);
void phw_bool(ph_writer *,const char *key,bool);
void phw_null(ph_writer *,const char *key);
void phw_value(ph_writer *,const char *key,const ph_value *);
void phw_value_canonical(ph_writer *,const char *key,const ph_value *);
/* Internal cache only: one already validated self-generated value, with true
 * maximum container depth. Never pass request bytes or aliased writer storage. */
void phw_cached_value(ph_writer *,const char *key,const ph_buf *,unsigned depth);
ph_buf *phw_take(ph_writer *); /* NULL on failure, frees partial output */
void phw_discard(ph_writer *);
