/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "http.h"
#include "../service.h"
#include "json_heap.h"
#include "printbuf.h"
/* One business lease, never one allocation per pending connection. */
typedef struct {
    unsigned claim;
    bool waiting, ota;
    char *body;
    size_t used, capacity;
    uint64_t failures;
    struct printbuf *wire;
    char scalar[1536];
} ph_http_product;
void ph_http_product_init(ph_http_product *, ph_http_exchange *);
void ph_http_busy(ph_http_exchange *);
void ph_http_message(ph_http_product *, ph_http_exchange *, unsigned, const char *);
void ph_http_json(ph_http_product *, ph_http_exchange *, json_object *);
ph_http_action ph_http_claim(ph_http_product *, ph_http_exchange *, size_t input, bool read_body);
bool ph_http_body_copy(ph_http_product *, const void *, size_t);
json_object *ph_http_body_json(ph_http_product *, ph_http_exchange *);
void ph_http_product_release(ph_http_product *);
