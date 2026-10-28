/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "typed_wire.h"

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
/* Immutable info payload, without a full status capture. */
void service_product_info_mp(ph_writer *output);

/* Invalidate full status and/or capability bytes after authoritative changes.
 * Safe before service_init. No capture, allocation or IO occurs here. */
enum { SERVICE_CACHE_STATUS = 1, SERVICE_CACHE_CAPABILITIES = 2,
       SERVICE_CACHE_ALL = 3 };
void service_cache_changed(unsigned mask);
