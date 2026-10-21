#pragma once
#include <stddef.h>
#include "http_trace_hooks.h"
/* Previous warm-boot HTTP trace only. Contains numeric stages, never payloads.
 * Unknown/cold/changed-build RAM is invalid; absence is not a no-fault proof. */
size_t tc1_http_trace_format(char *output, size_t capacity);
