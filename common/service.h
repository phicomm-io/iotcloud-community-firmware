#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "json_build.h"
#include "memory_budget.h"
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
void service_memory_snapshot(service_memory_state_t *out);
/* MQTT owner reports only scalar queue diagnostics; no allocation or socket IO. */
void service_memory_mqtt(unsigned pending, bool overflow, uint32_t delay_ms);
uint64_t service_memory_failures(void);
/* Product adapter: only static action names and validated parameters reach it. */
json_object *service_product(const char *, json_object *, const char **code);
json_object *service_product_capabilities(void);
