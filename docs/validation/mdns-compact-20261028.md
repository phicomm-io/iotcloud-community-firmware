# Shared mDNS error reporting

The pinned MiCO mDNS implementation is already a responder/announcer, without an
active DNS-SD browser. Its response-receive branch is empty. There is no browser
code to remove. The old A1 image did not retain this SDK object; LAN registration
now retains 3754 bytes (3616 text, 134 read-only data, 4 initialized data).
Existing HTTP, Wi-Fi and pairing do not provide another always-running mDNS
implementation. The singleton and records are shared with EasyLink when used.

This overlay only shares the repeated SDK DEBUG assertion output body. The SDK
macros expand stdio locking, the clock read, printf and unlocking at multiple
failure branches. One explicitly out-of-line helper retains file, caller, line,
message and the runtime debug switch. All error checks, error codes, cleanup,
mutexes, network and announcement state machines are unchanged. The pinned
debug macros do not print their A/B assertion-expression arguments.

The overlay is installed after the existing atomic-add/multi-record-resume
overlay using the ordered, idempotent SDK patch installer. No SDK cache or
original matrix source/object/log is modified by the isolated trials.

Actual GCC 5.4.1 relinks with model pruning, compact diagnostic helpers and
unused forced float printf removed:

| Product | Before | After | Saving | Remaining overflow |
| --- | ---: | ---: | ---: | ---: |
| A1 fresh-air | 263796 | 263460 | 336 | 1316 |
| M1 | 265204 | 264868 | 336 | 2724 |
| TC1 | 217772 | 217436 | 336 | 0 |

A1 and M1 still fail the unchanged flash limit and have no trial ELF/OTA.
Actual map/object/`.su` evidence and result hashes are under the independent
checkout's `artifacts/space-status-float/mdns-compact-target/`. The new helper
uses 32 bytes of actual GCC5 stack on error paths; `mdns_add_record` uses 120.
The SDK path remains inside the existing fixed SDK reserve; no reserve or task
budget is reduced. Full product stack acceptance requires the clean matrix.

Four ASAN/UBSAN host tests pass: ordered overlay idempotence, identical actual
SDK debug-macro output with RTOS and without RTOS, runtime logging disable,
unchanged protocol/lifecycle function bodies, four allocation failures,
announcement-thread failure, atomic record preservation, multi-record resume
and TXT canaries. Build script syntax and whitespace checks pass.

No announcement/query, PTR/SRV/TXT/A record, Station/Soft_AP interface, TTL,
reconnect notification, EasyLink consumer or thread is removed. Existing SDK
limitations remain: response conflict processing is empty; QU/legacy unicast
handling is incomplete; compression-name parsing is not bounded. This narrow
space patch neither introduces nor fixes these limitations and is not evidence
of complete mDNS standards conformance or physical discovery acceptance.

The subsequent integrated A1/M1 GCC5/GCC14 rebuilds, compiler provenance,
packaging correction and remaining physical acceptance are recorded in
[the 2026-10-29 matrix](mdns-gcc-matrix-20261029.md).
