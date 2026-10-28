# Native MessagePack service regressions

`python3 -m unittest tests.test_service_mp` compiles the actual `common/service.c`
translation unit, borrowed reader and direct writer. `NATIVE_MP_ROOT` optionally
selects a separate source checkout. `SANITIZE=1` repeats the same fixtures under
ASan and UBSan. `MOC=1` selects the production heap admission/allocation branch;
`test_heap_free` injects the SDK's instantaneous free-memory report. Allocator
wrapping accounts for every direct application allocation, including retained
cache and receipt descriptor/storage pairs. It does not model SDK/RTOS or
shared-libcrypto internal allocation, physical transport, or actuator effects.

The OTA-active fixture includes the unchanged production dispatcher with a tiny
job backend; it tests dispatch gates, not the OTA transfer/install worker.
Recovery hooks are strong symbols in a separate translation unit.

## Retained assertion mapping

| Original assertion | Actual MessagePack regression |
| --- | --- |
| `test_service.c`: status query, malformed/extra fields, replay/conflict | Existing malformed/request-ID/cache tests, plus `test_embedded_nul_null_params_and_trailing_bytes_are_rejected`; real binary input with NUL, null params and trailing document bytes is rejected without capture or mutation. |
| `test_service.c`: stale errno, logs limit 8/9, bounded paging, overwrite flag, redaction, dictionary and signed 32-bit extremes | `test_logs_paging_redaction_dictionary_boundaries_and_bad_types`; asserts oldest-page cursor progression, ring overwrite, rejection of unknown/secret events, exact INT_MIN/MAX event strings, rejection outside the range and limit/after wrong types. The same assertion runs with both native host strtol and saturating ILP32 strtol emulation to catch 32-bit overflow despite stale errno. |
| `test_service_failure.c`: before-execution failure can retry, executed mutations never repeat, same ID changed data conflicts | Existing 12 allocation-cutpoint test plus `test_explicit_receipt_copy_oom_and_bounded_eviction`; specifically fails both saved-response allocations while preserving returned accepted output and a busy replay tombstone. |
| `test_service_failure.c`: incomplete post-mutation output cannot replay the operation | `test_live_heap_pressure_incomplete_output_and_post_mutation_tombstone`; drops real MOC heap after the first emitted member, forces subsequent buffer-growth failure, requires NULL output and busy same-ID retry with exactly one mutation. |
| `test_service_failure.c`: 100 bounded receipt evictions | `test_explicit_receipt_copy_oom_and_bounded_eviction`; exact live bytes/blocks stay fixed after eight entries, most recent replay is retained, oldest evicted ID can execute again. |
| `test_service_memory.c`: 10,000 cleanup cycles, active/reserved reset | `test_ten_thousand_owned_cleanup_cycles_keep_only_fixed_caches`; all 10,000 iterations preserve exact live blocks/bytes with both eight receipts and two caches already populated, with allocations minus frees equal to live blocks. |
| `test_service_memory.c`: live low heap blocks output then recovers | `test_live_heap_pressure_incomplete_output_and_post_mutation_tombstone`; rejects allocation at the production reserve, frees partial output, recovers status at 64,000 free bytes. |
| `test_service_memory.c`: 7 KiB request replay, changed final byte conflicts, no retained input | `test_large_binary_params_replay_digest_and_no_retained_input`; sends eight 7 KiB binary parameters, poisons/frees each request, accounts for only the exact small saved responses, repeats without mutation, and changes the last byte under the same ID. |
| `test_service_memory.c`: 64 simultaneous claims are byte-admitted, low heap rejects, diagnostics stable | `test_live_byte_admission_sixty_four_claims_and_diagnostics`; asserts 64 active claims, exact reserved bytes, underflow-safe release/waiting, input-size and heap denials, recovery, minimum free, pending/peak/overflow/max-delay MQTT counters. |

Old SDK JSON graph allocator and serialize-pass implementation assertions are
legacy tests; the binary dispatcher has neither that graph nor sizing passes.
The old `service_memory_failures()` assertions map to the independent
`wire_alloc_failed` and `wire_guard_denied` diagnostics: allocator OOM increments
only the first, while a live heap reserve rejection increments only the second.
Legacy JSON counters stay compatible but do not measure binary allocation.

## Real OTA metadata worker

`OtaMetadataWorkerService` compiles the entire actual service.c with OTA enabled.
The test replaces only SDK worker scheduling, job reservation and fetch/install
boundaries. A queued worker runs after its source request was poisoned/freed.
It consumes the actual publisher MessagePack golden and executes the production
metadata parser/version comparison. Both `/ota/candidate` and `/ota/v1` builds
assert exact requested URLs, lowercase URL model versus uppercase manifest model,
newer/equal/older release availability, signed-only install paths, cleared stale
availability after failure and exactly one selected-source fetch without fallback.
It does not test TLS, firmware signature verification or physical installation.
