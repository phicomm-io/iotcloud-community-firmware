# Compact diagnostics field writers

Four out-of-line typed helpers share the existing `[id, value]` begin/ID/value/end
sequence. IDs, conditions, ordering, signed conversions, strings and writer
failure behavior remain unchanged. The model specialization is a separate
parent commit. No fields, buffers, stack budgets or SDK reserves are removed.

Actual GCC 5.4.1 isolated relinks against immutable cc9ca4af input archives,
including the model specialization and removal of unused forced float printf:

| Product | Before helpers | After helpers | Delta | Remaining overflow |
| --- | ---: | ---: | ---: | ---: |
| A1 fresh-air | 264516 | 263796 | -720 | 1652 |
| M1 | 266132 | 265204 | -928 | 3060 |
| TC1 | 217996 | 217772 | -224 | 0 |

These A1/M1 trial links still fail the fixed flash limit and produce no ELF/OTA.
The outputs are at `artifacts/space-status-float/helper-target/results.json`
and the sibling per-product map, object, `.su` and build log files in the
independent `space-status-float` checkout. No matrix artifact or compiler
dispatch log is written by these direct-toolchain relinks.

The actual helper frames are 16 bytes each on this compiler. The diagnostics
visitor remains 32/72/16 bytes for A1/M1/TC1. `diagnostic_field` is an additional
16-byte call where emitted on A1/M1; it inlines into the typed helper on TC1.
`diagnostic_paths()` charges the helper on both its pair and value tails.
Sibling helpers are never summed together. An emitted helper without an actual
`.su` record fails closed. Full product stack gates still need the next clean
matrix; these object frames alone are not a complete stack acceptance result.

Validation: 30 tests passed with `SANITIZE=1`, covering actual old/new writer
bytes for all three models and five feature combinations, 32 diagnostic states,
measure/fixed buffers and canaries, allocation failure, sticky/null behavior,
and HTTP/MQTT/worker/LAN stack gates including missing-frame/overflow controls.
No DUT operation or performance claim is made.
