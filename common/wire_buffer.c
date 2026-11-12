/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wire_buffer.h"
#include <stdlib.h>
#include <string.h>
__attribute__((weak)) void *ph_wire_malloc(size_t size) { return malloc(size); }
__attribute__((weak)) void ph_wire_free(void *p) { free(p); }
ph_buf *ph_buf_new(size_t capacity)
{
    if(!capacity)return NULL;
    ph_buf *b=ph_wire_malloc(sizeof(*b));
    if(!b)return NULL;
    b->buf=ph_wire_malloc(capacity);
    if(!b->buf){ph_wire_free(b);return NULL;}
    b->len=0;b->cap=capacity;b->buf[0]=0;return b;
}
bool ph_buf_reserve(ph_buf *b,size_t capacity)
{
    if(!b || !b->buf || b->len>=b->cap)return false;
    if(capacity<=b->cap)return true;
    char *next=ph_wire_malloc(capacity);
    if(!next)return false;
    memcpy(next,b->buf,b->len+1);ph_wire_free(b->buf);
    b->buf=next;b->cap=capacity;return true;
}
void ph_buf_free(ph_buf *b)
{
    if(b){ph_wire_free(b->buf);ph_wire_free(b);}
}
