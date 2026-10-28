# RC4 regression migration inventory

Baseline: `2fbe51b`. A passing native subset is not a full-regression result.
The original `tests/run.sh` remains the full gate until all rows below are closed.
No product case is moved into the historical SDK-only group to hide a failure.

| Old fixture / assertions | Native equivalent | Owner / current gap |
| --- | --- | --- |
| `test_engine.c`, `test_sensor_aggregate.c`: PID, hysteresis, statistics, sleep/schedule, rollover, partial-field/CO2 freshness | Same unmodified C bodies, `test_native_features` | features; preserve in full |
| `test_automation.c`, `countdown_scenarios.inc`: IDs, persistence-before-action, reboot-last, no replay/catch-up, busy/missed/capacity, all button gestures/pagination, A1 uptime countdown cancel/restart/edit/reboot/wrap, M1 weekly alarms/tone/bounds | Same scenarios and assertions now call `automation_mp` with build-time constant MP vectors; responses use borrowed `ph_value`; `test_native_features` runs A1/M1/TC1 | features; native + ASan/UBSan PASS |
| `test_runtime.c` transaction validation / CAS / flash rejection / binding limits / six domains | `test_config_mp.c`, `test_feature_mp.c`; historical validation vectors, four/five peer limit, legacy-clamped reads, actual install sample remapping/credential change/save rejection added | features; do not equate existing short MP smoke coverage with all 1049 old lines |
| `test_runtime.c` applied fan snapshot, day/night caps/fallback, manual resume, light sleep, animation speed/effect boundaries | Extract real runtime functions with unchanged historical assertion bodies, native command inputs | features; native + ASan/UBSan PASS |
| `test_runtime.c` group ACK/timeout/retry ownership/auth/replay, `passive_scenarios.inc`, sensor grant | `test_group_mp`, `test_lan_wire`, `test_lan_client`, `test_sensor_grant_mp` | auth owner must confirm old-to-new scenario equivalence |
| `test_runtime.c` generic sensor MQTT/display bridge | `test_m1_display_mp`, `test_sensor_producers_mp`; schema3 metadata + snapshot supersedes schema2 repeated descriptors | protocol owner; explicit remaining scenario audit requested via decimal owner |
| `test_runtime.c` account namespace/filter bounds/coalescing/stale generations, old MQTT receipts/preflight/first-cause tests | `test_runtime_mqtt_mp`, `test_native_mqtt` plus actual shared Paho lifecycle tests | decimal owner migrating gaps; not complete |
| `test_runtime.c` HA identity/names/optional entity cleanup; `test_mqtt_discovery_budget` | `test_runtime_mqtt_mp` fixed tiny JSON producer max-size, cursor/transport retry and actual parsed HA payloads; retain identity/layout/name/cursor/OOM cases | features + decimal owner; gap audit pending |
| old `test_service{,_failure,_memory}.c`, HTTP JSON callbacks/owned/budget/background/status fixtures | `test_service_mp`, `test_http_product_mp`, native callback/fixed busy/MP stack/cache/status oracles | typed owner; service/HTTP/status equivalence migration in progress |
| backup/recovery/storage/pairing/auth fixtures | native backup MP `PHCBACK1` + actual persistent snapshot layout + auth/session cases | auth owner; mapping requested |
| Bemfa and product MQTT/HA/TC1 old fixtures | `test_bemfa_mp`, native TC1 business/HA + shared native MQTT | decimal owner; full old scenario audit in progress |
| pure pinned JSON SDK allocation/parse/number/printbuf/ABI tests | Explicit historical SDK-only group, not a dependency of native product business fixtures | runner owner; `tests/run.sh --legacy-sdk` separates host SDK library tests; ARM/QEMU history uses `LEGACY_SDK_ARM=1` |

## Host vectors

`mp_fixture_vectors.py` uses Python's JSON parser only while generating constant
MP byte arrays before C compilation. The native automation test executable links
`typed_wire.c` and `wire_buffer.c`; it does not include/link JSON objects or a
runtime format bridge. Dynamic page/weekly requests use direct typed writers.

## Evidence boundary

Native compilation and sanitizer execution do not prove full firmware link,
DUT installation, concurrency P95, busy recovery, or physical-button acceptance.

## Executable entries (this branch)

- `SANITIZE=1 bash tests/run.sh --native-common`: 9 unittest cases; automation
  compiles and executes three model variants. No SDK/JSON objects are linked.
- `PHICOMM_TEST_JSON_DIR=... bash tests/run.sh --legacy-sdk`: explicit historical
  SDK JSON library-only coverage. ARM cache/stack history is opt-in via
  `LEGACY_SDK_ARM=1`; it is not represented by the host-only result.
- `bash tests/run.sh --all`: preserved full gate. Other owners are still
  migrating old product/HTTP/MQTT fixtures; no full PASS is claimed.

The runtime controls test extracts the current production implementation on every
run, including `install_config`, applied-state/PID/manual/cap/animation functions
and `feature_mp.inc`. Historical assertion bodies live in
`runtime_control_scenarios.inc`; allocator, persistence, RTOS locks and group
transport are seams. The separate group/LAN modules own packet-level evidence.
