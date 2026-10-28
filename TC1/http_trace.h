#pragma once
#include "../common/typed_wire.h"
#include "http_trace_hooks.h"
/* Previous warm-boot HTTP trace only. Contains numeric stages, never payloads.
 * Unknown/cold/changed-build RAM is invalid; absence is not a no-fault proof. */
void tc1_http_trace_mp(ph_writer *);
