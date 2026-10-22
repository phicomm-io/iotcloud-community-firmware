#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "json_build.h"
#include "memory_budget.h"
/* Serialize internal typed controls with transport commands, without JSON. */
bool service_control_begin(void);
void service_control_end(void);
/* All transports share this dispatcher. The returned object belongs to caller. */
void service_init(void);
json_object *service_parse(const void *, size_t);
json_object *service_command(json_object *);
json_object *service_query(const char *);
void service_event(const char *level, const char *module, const char *event);
unsigned service_memory_claim(size_t input_bytes);
void service_memory_release(unsigned claim);
void service_memory_waiting(bool enter);
/* Non-waiting exclusion for graph construction through owned encoding. Never
 * retain a token across socket IO/PUBACK; a stale release cannot clear its heir. */
unsigned service_json_claim(void);
void service_json_release(unsigned token);
/* One bounded attempt within a live JSON lease. known_owner is a stable MiCO
 * mico_thread_t*, tested with the exported is_current_thread API. No scope may
 * cross socket I/O. Optional publishers may scope their encoding; command
 * callers end before encoding and rely on the encoder's own graph checks. */
bool service_json_scope_begin(unsigned token, void *known_owner);
unsigned service_json_scope_failed(unsigned token);
unsigned service_json_scope_end(unsigned token);
/* Scoped caller failures, or the original global comparison for legacy callers. */
bool service_caller_graph_failed(uint64_t global_before);
void service_memory_snapshot(service_memory_state_t *out);
/* MQTT owner reports only scalar queue diagnostics; no allocation or socket IO. */
void service_memory_mqtt(unsigned pending, bool overflow, uint32_t delay_ms);
uint64_t service_memory_failures(void);
/* Product adapter: only static action names and validated parameters reach it. */
json_object *service_product(const char *, json_object *, const char **code);
json_object *service_product_capabilities(void);

/* Borrow input, return independent wire bytes (printbuf_free). */
struct printbuf *service_command_wire(json_object *);
/* MQTT-only product actions bypass the general replay cache (may contain secrets). */
struct printbuf *service_mqtt_command_wire(json_object *);
struct printbuf *service_query_wire(const char *);
struct printbuf *service_product_status_wire(bool live, bool envelope, const char *request_id);
