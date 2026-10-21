/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "json.h"
/* Consume one owned reference, returning independent wire bytes or NULL.
 * Unique container subtrees may be emptied. Shared ancestor subtrees remain
 * unchanged. The caller frees the result with printbuf_free after transport.
 * Graphs must be acyclic JSON values; nesting beyond 32 nodes fails closed.
 * Cycles are rejected by the depth limit, not reclaimed by reference counting. */
enum json_owned_error { JSON_OWNED_OK, JSON_OWNED_RESOURCE, JSON_OWNED_GRAPH, JSON_OWNED_INVALID };
struct printbuf *json_owned_to_wire_result(json_object *, enum json_owned_error *);
struct printbuf *json_owned_to_wire(json_object *);
/* Consume one reference before calling a synchronous transport. Only independent
 * wire bytes survive callback re-entry/PUBACK waiting. -1 means encoding failed;
 * otherwise preserve the callback's return value, topic and retain policy. */
int json_owned_publish(int (*)(const char *, const char *, bool),
                       const char *, json_object *, bool);
