/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
/* len is wire length. cap includes one trailing sentinel byte, never payload. */
typedef struct { char *buf; size_t len,cap; } ph_buf;
/* Override weak defaults for device budget/admission or deterministic OOM tests.
 * Both functions must belong to the same allocator and remain thread-safe. */
void *ph_wire_malloc(size_t);
void ph_wire_free(void *);
ph_buf *ph_buf_new(size_t capacity);
bool ph_buf_reserve(ph_buf *,size_t capacity);
void ph_buf_free(ph_buf *);
