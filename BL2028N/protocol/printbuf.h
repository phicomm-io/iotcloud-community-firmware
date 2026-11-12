/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdlib.h>
struct printbuf { char *buf; int bpos, size; };
static inline void printbuf_free(struct printbuf *p) {
    if (p) { free(p->buf); free(p); }
}
