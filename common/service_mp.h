/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "typed_wire.h"
#include "memory_budget.h"
void service_init(void);
void service_event(const char *level,const char *module,const char *event);
bool service_control_begin(void);
void service_control_end(void);
/* Admission only, before command/control dispatch: may evict expendable caches
 * under the command mutex. Never call while holding a service_control lease. */
unsigned service_memory_claim(size_t input_bytes);
void service_memory_release(unsigned claim);
void service_memory_waiting(bool enter);
void service_memory_snapshot(service_memory_state_t *out);

/* Request memory is borrowed for this call only. The returned wire buffer is
 * independently owned; release it with ph_buf_free(), after transport IO. All
 * asynchronous business adapters must copy the validated typed parameters. */
ph_buf *service_command_mp(const void *data, size_t length);
ph_buf *service_mqtt_command_mp(const void *data, size_t length);
ph_buf *service_query_mp(const char *action);
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
