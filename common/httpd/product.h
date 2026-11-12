/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "http.h"
#include "../service_mp.h"
#ifndef PH_HTTP_SCALAR_BYTES
#define PH_HTTP_SCALAR_BYTES 1536
#endif
/* One bounded business lease, independent of pending connection slots. */
typedef struct {
 unsigned claim;
 bool waiting,ota;
 char *body;
 size_t used,capacity;
 ph_buf *wire;
 ph_auth_context auth;
#if defined(PHICOMM_STATUS_MODEL) && PHICOMM_STATUS_MODEL == 2
 void *control_credit;
#endif
 char scalar[PH_HTTP_SCALAR_BYTES];
} ph_http_product;
void ph_http_product_init(ph_http_product *,ph_http_exchange *);
void ph_http_busy(ph_http_exchange *);
void ph_http_message(ph_http_product *,ph_http_exchange *,unsigned,const char *);
ph_http_action ph_http_claim(ph_http_product *,ph_http_exchange *,size_t,bool);
bool ph_http_body_copy(ph_http_product *,const void *,size_t);
void ph_http_product_release(ph_http_product *);
void ph_http_wire_take(ph_http_product *,ph_http_exchange *,ph_buf *);
void ph_http_command_mp(ph_http_product *,ph_http_exchange *,const void *,size_t);
void ph_http_query(ph_http_product *,ph_http_exchange *,const char *);
#if defined(PHICOMM_STATUS_MODEL) && PHICOMM_STATUS_MODEL == 2
ph_http_action ph_http_control_claim(ph_http_product *,ph_http_exchange *,size_t);
bool ph_http_control_command(ph_http_product *,ph_http_exchange *);
#endif
