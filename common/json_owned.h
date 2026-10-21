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
struct printbuf *json_owned_to_wire(json_object *);
