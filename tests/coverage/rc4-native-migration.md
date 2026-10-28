# RC4 regression migration inventory

Baseline: `2fbe51b`. A passing native subset is not a full-regression result.
The original `tests/run.sh` remains the full gate until all rows below are closed.
No product case is moved into the historical SDK-only group to hide a failure.

| Old fixture / assertions | Native equivalent | Owner / current gap |
| --- | --- | --- |
| `test_watchdog.c`: mandatory worker deadlines, latched failure, startup/clock wrap, network/OTA bounds, Flash progress and failure, cached boot reasons, monitor failures | Same full original assertions and actual watchdog capture + native status fields | features; ASan/UBSan PASS |
| `test_a1_control.c`: button/slider FIFO, overflow, expiry/wrap, lock/toggles, OTA/provisioning busy, reset and invalid transport params | Same full original assertions against actual typed control and automation; all wire allocations are denied and asserted unused | features; ASan/UBSan PASS |
| `test_runtime_metrics.c`: missing heap, sampled minimum, consuming boot-reason cache and tick wrap on portable/MOC | Same original assertions encode native `device_runtime_mp`; both variants in `test_native_features` | features; ASan/UBSan PASS |
| `test_engine.c`, `test_sensor_aggregate.c`: PID, hysteresis, statistics, sleep/schedule, rollover, partial-field/CO2 freshness | Same unmodified C bodies, `test_native_features` | features; preserve in full |
| `test_automation.c`, `countdown_scenarios.inc`: IDs, persistence-before-action, reboot-last, no replay/catch-up, busy/missed/capacity, all button gestures/pagination, A1 uptime countdown cancel/restart/edit/reboot/wrap, M1 weekly alarms/tone/bounds | Same scenarios and assertions now call `automation_mp` with build-time constant MP vectors; responses use borrowed `ph_value`; `test_native_features` runs A1/M1/TC1 | features; native + ASan/UBSan PASS |
| `test_runtime.c` transaction validation / CAS / flash rejection / binding limits / six domains | `test_config_mp.c`, `test_feature_mp.c`; historical validation vectors, four/five peer limit, legacy-clamped reads, actual install sample remapping/credential change/save rejection added | features; do not equate existing short MP smoke coverage with all 1049 old lines |
| `test_runtime.c` applied fan snapshot, day/night caps/fallback, manual resume, light sleep, animation speed/effect boundaries | Extract real runtime functions with unchanged historical assertion bodies, native command inputs | features; native + ASan/UBSan PASS |
| `test_runtime.c` group ACK/timeout/retry ownership/auth/replay, `passive_scenarios.inc`, sensor grant | `test_group_mp`, `test_lan_wire`, `test_lan_client`, `test_sensor_grant_mp` | auth owner must confirm old-to-new scenario equivalence |
| `test_runtime.c` generic sensor MQTT/display bridge | `test_m1_display_mp`, `test_sensor_producers_mp`, `test_sensor_mp`; real producer/consumer schema3 metadata + snapshot, strict full-u32 counters, descriptor/unit/TTL/replay/reconnect/borrowed-buffer checks | protocol owner; ASan/UBSan PASS. `test_runtime_session_mp` preserves actual M1 ingress/session wiring, namespace/offline notifications/native grant dispatch |
| `test_runtime.c` account namespace/filter bounds/coalescing/stale generations, old MQTT receipts/preflight/first-cause tests | `test_runtime_mqtt_mp`, `test_native_mqtt` plus actual shared Paho lifecycle tests | decimal owner migrating gaps; not complete |
| `test_runtime.c` HA identity/names/optional entity cleanup; `test_mqtt_discovery_budget` | `test_runtime_mqtt_mp` fixed tiny JSON producer max-size, cursor/transport retry and actual parsed HA payloads; retain identity/layout/name/cursor/OOM cases | features + decimal owner; gap audit pending |
| old `test_service{,_failure,_memory}.c`, HTTP JSON callbacks/owned/budget/background/status fixtures | `test_service_mp`, `test_http_product_mp`, native callback/fixed busy/MP stack/cache/status oracles | typed owner; service assertions migrated in `tests/service_mp/README.md`; callback/capture equivalence and residual ARM32 heap limits in `tests/http_core/NATIVE_COVERAGE.md` |
| backup/recovery/storage/pairing/auth fixtures | native backup MP `PHCBACK1` + actual persistent snapshot layout + auth/session cases | auth owner; backup, ordinary recovery, frozen layout and session modules passed; old passive simulator scenario audit remains open |
| Bemfa and product MQTT/HA/TC1 old fixtures | `test_bemfa_mp`, native TC1 business/HA + shared native MQTT | decimal owner; full old scenario audit in progress |
| `test_a1_fresh_air.py`: actual controller GPIO/PWM, sensor fallback, pulses/RPM, bounds/revisions, snapshot purity, PID saturation, independent coupling, shutdown | Same full assertions, build-time constant MP request vectors plus a direct writer for dynamic revision | features; ASan/UBSan PASS without JSON headers or library |
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

- `SANITIZE=1 bash tests/run.sh --native-common`: 12 unittest cases; automation
  compiles and executes three model variants. No SDK/JSON objects are linked.
- `PHICOMM_TEST_JSON_DIR=... bash tests/run.sh --legacy-sdk`: explicit historical
  SDK JSON library-only coverage. ARM cache/stack history is opt-in via
  `LEGACY_SDK_ARM=1`; it is not represented by the host-only result.
- `SANITIZE=1 bash tests/run.sh --native-rc4`: explicit expanded native migration
  subset. Requires pinned SDK paths for actual Paho and M1 Flash fixtures; missing
  prerequisites are failures, and any skip makes the launcher fail. It generates
  the version header before compilation. See `tests/run_native_rc4.py` for the
  exact list, including isolated HA JSON-output oracle tests.
- `ARM_CC=... bash tests/run.sh --native-arm`: mandatory pinned ARM32 compiler
  + qemu execution (no skips), separate from the host subset. Actual linked
  native status/codec allocator-ledger ownership/OOM/recovery, all three models.
  This does not measure closed-MiCO-allocator or maximum full-runtime peaks.
- `bash tests/run.sh --all`: preserved full gate. Other owners are still
  migrating old product/HTTP/MQTT fixtures; no full PASS is claimed.

The runtime controls test extracts the current production implementation on every
run, including `install_config`, applied-state/PID/manual/cap/animation functions
and `feature_mp.inc`. Historical assertion bodies live in
`runtime_control_scenarios.inc`; allocator, persistence, RTOS locks and group
transport are seams. The separate group/LAN modules own packet-level evidence.

## Remaining gates

- Original full `test_runtime.c` remains in `--all` until the auth/UDP owner has
  mapped passive-link/sensor-session pacing and all historical group simulator
  scenarios. Current native group/LAN tests are not presumed equivalent.
- Shared MQTT receipt/preflight/filter/first-cause cases remain owned by the
  MQTT migration lane; original gates are retained pending that complete map.
- The native ARM32 codec ownership/allocator ledger is a separate job. Its
  bounded fixture does not establish closed-SDK concurrent runtime heap peaks.
- Full `--all` still contains stale JSON-product fixtures. Their presence is an
  explicit unresolved release gate, not a passed test or an SDK legacy exemption.

Expanded subset verified on this branch with `SANITIZE=1`: **92 tests PASS,
zero skipped**, 56.247 s (`/tmp/rc4-native-expanded-ready.log`). The actual Paho
fixture used `PHICOMM_TC1_SDK` and actual M1 Flash recovery used `M1_SDK_DIR`,
both pointing to existing pinned SDK checkouts; fixtures archive/extract code
into disposable directories and do not patch those checkouts. This is host
regression evidence only. It does not close the remaining gates above.

The native ARM entry was executed successfully using pinned GCC 5.4.1/newlib
nano and qemu: A1/M1 retained 2824 B, modeled overlap peak 7472 B; TC1 retained
768 B, modeled overlap peak 1992 B. OOM sweeps preserve the first response and
all models return to zero owned bytes. Log: `/tmp/rc4-native-arm-entry.log`.
After the runtime metrics migration, `--native-common` was rerun: 10 tests PASS
in 7.039 s (`/tmp/rc4-native-common-final.log`).

The legacy SDK host entry was rerun after isolating integer-width vectors:
5 Python library tests + 3 C binaries PASS; final leak ledger live=0 after
10000 serial and 4000 concurrent cycles. `/tmp/rc4-legacy-sdk-final.log`.
