#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "json.h"
/* All transports share this dispatcher. The returned object belongs to caller. */
void service_init(void);
json_object *service_parse(const void *, size_t);
json_object *service_command(json_object *);
json_object *service_query(const char *);
void service_event(const char *level, const char *module, const char *event);
/* Product adapter: only static action names and validated parameters reach it. */
json_object *service_product(const char *, json_object *, const char **code);
json_object *service_product_capabilities(void);
