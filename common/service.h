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
void service_memory_snapshot(service_memory_state_t *out);
/* Product adapter: only static action names and validated parameters reach it. */
json_object *service_product(const char *, json_object *, const char **code);
json_object *service_product_capabilities(void);
