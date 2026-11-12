/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "typed_wire.h"
#include "memory_budget.h"
#include "local_auth.h"
void service_init(void);
void service_event(const char *level,const char *module,const char *event);
bool service_control_begin(void);
void service_control_end(void);
/* Admission only, before command/control dispatch: may evict expendable caches
 * under the command mutex. Never call while holding a service_control lease. */
unsigned service_memory_claim(size_t input_bytes);
/* LAN owner never waits for cache eviction/another command as admission. */
unsigned service_memory_try_claim(size_t input_bytes);
void service_memory_release(unsigned claim);
void service_memory_waiting(bool enter);
void service_memory_snapshot(service_memory_state_t *out);

/* Request memory is borrowed for this call only. The returned wire buffer is
 * independently owned; release it with ph_buf_free(), after transport IO. All
 * asynchronous business adapters must copy the validated typed parameters. */
ph_buf *service_command_mp(const void *data, size_t length);
/* HTTP retains its original admin context across waits. Recheck it after the
 * blocking command admission, before receipts, cache reads or mutation. */
ph_buf *service_http_command_mp(const void *,size_t,const ph_auth_context *);
ph_buf *service_http_query_mp(const char *,const ph_auth_context *);
#if defined(PHICOMM_STATUS_MODEL) && PHICOMM_STATUS_MODEL == 2
/* TC1's three small controls have an empty data map. Materialize the receipt
 * before reading their request; the reply uses the HTTP owner's scalar.
 * Other actions keep the ordinary dispatcher. A consumed credit belongs to
 * the existing receipt table, otherwise the HTTP owner releases it. */
#define SERVICE_HTTP_CONTROL_INPUT 256u
#define SERVICE_HTTP_CONTROL_REPLY 160u
void *service_http_control_credit(void);
void service_http_control_credit_free(void *);
bool service_http_control_mp(const void *,size_t,const ph_auth_context *,
                             void **credit,void *reply,size_t,size_t *length,
                             unsigned *status);
#endif
ph_buf *service_mqtt_command_mp(const void *data, size_t length);
/* Authenticated UDP business only; fixed allowlist excludes administration.
 * Context is checked again under the command mutex before receipts/mutation. */
ph_buf *service_lan_command_mp(const void *data,size_t length,const ph_auth_context *);
ph_buf *service_lan_try_command_mp(const void *,size_t,const ph_auth_context *,bool *admitted);
ph_buf *service_query_mp(const char *action);
/* Same validated .get classification as dispatch; never admits a mutation. */
bool service_readonly_mp(const void *,size_t);
ph_buf *service_error_mp(const char *request_id, const char *code);

/* Product emits exactly one data value (usually a map). Returning false means
 * unhandled, with no output and no side effects. On handled validation errors,
 * set code and emit an empty map. Every mutation runs at most once. */
bool service_product_mp(const char *action, const ph_value *params,
                        ph_writer *output, const char **code);
bool service_product_mqtt_mp(const char *action, const ph_value *params,
                             ph_writer *output, const char **code);
void service_product_capabilities_mp(ph_writer *output,
                                    const char *const *extra, bool restricted);
void service_product_status_mp(ph_writer *output);
void service_product_diagnostics_mp(ph_writer *output);
/* Immutable info payload, without a full status capture. */
void service_product_info_mp(ph_writer *output);
bool service_product_ota_start(const char *url);

/* Invalidate full status and/or capability bytes after authoritative changes.
 * Safe before service_init. No capture, allocation or IO occurs here. */
enum { SERVICE_CACHE_STATUS = 1, SERVICE_CACHE_CAPABILITIES = 2,
       SERVICE_CACHE_ALL = 3 };
void service_cache_changed(unsigned mask);
/* Cheap version only: never captures, allocates, or acknowledges a channel. */
uint32_t service_cache_revision(void);
