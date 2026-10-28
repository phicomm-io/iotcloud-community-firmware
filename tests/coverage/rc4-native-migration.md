# RC4 regression migration inventory

Baseline: `2fbe51b`. A passing native subset is not a full-regression result.
The default `tests/run.sh --all` combines the explicit native manifest with the
retained transport/tooling/hardware/recovery/OTA/Web gates. The previous full
script and its old implementation fixtures remain available through
`--historical-product`; the pure JSON SDK library has its own `--legacy-sdk` job.
The device JSON compatibility contract was intentionally removed: deleted DOM
implementation assertions do not require a production compatibility adapter.
Business assertions are replaced only by the explicit mappings below.

| Old fixture / assertions | Native equivalent | Owner / current gap |
| --- | --- | --- |
| `test_watchdog.c`: mandatory worker deadlines, latched failure, startup/clock wrap, network/OTA bounds, Flash progress and failure, cached boot reasons, monitor failures | Same full original assertions and actual watchdog capture + native status fields | features; ASan/UBSan PASS |
| `test_a1_control.c`: button/slider FIFO, overflow, expiry/wrap, lock/toggles, OTA/provisioning busy, reset and invalid transport params | Same full original assertions against actual typed control and automation; all wire allocations are denied and asserted unused | features; ASan/UBSan PASS |
| `test_runtime_metrics.c`: missing heap, sampled minimum, consuming boot-reason cache and tick wrap on portable/MOC | Same original assertions encode native `device_runtime_mp`; both variants in `test_native_features` | features; ASan/UBSan PASS |
| `test_engine.c`, `test_sensor_aggregate.c`: PID, hysteresis, statistics, sleep/schedule, rollover, partial-field/CO2 freshness | Same unmodified C bodies, `test_native_features` | features; preserve in full |
| `test_automation.c`, `countdown_scenarios.inc`: IDs, persistence-before-action, reboot-last, no replay/catch-up, busy/missed/capacity, all button gestures/pagination, A1 uptime countdown cancel/restart/edit/reboot/wrap, M1 weekly alarms/tone/bounds | Same scenarios and assertions now call `automation_mp` with build-time constant MP vectors; responses use borrowed `ph_value`; `test_native_features` runs A1/M1/TC1 | features; native + ASan/UBSan PASS |
| `test_runtime.c` transaction validation / CAS / flash rejection / binding limits / six domains | `test_config_mp.c`, `test_feature_mp.c`; historical validation vectors, four/five peer limit, legacy-clamped reads, actual install sample remapping/credential change/save rejection added | features; common native config/control, UDP, sensor and account/HA rows below split the original matrix; no single smoke test substitutes for the original |
| `test_runtime.c` applied fan snapshot, day/night caps/fallback, manual resume, light sleep, animation speed/effect boundaries | Extract real runtime functions with unchanged historical assertion bodies, native command inputs | features; native + ASan/UBSan PASS |
| `test_runtime.c` group ACK/timeout/retry ownership/auth/replay, `passive_scenarios.inc`, sensor grant | `test_group_mp`, `test_lan_wire`, `test_lan_client`, `test_sensor_grant_mp` | auth owner; complete timing/OOM/recovery mapping in `tests/native_udp_runtime/COVERAGE.md`; native + ASan/UBSan PASS |
| `test_runtime.c` generic sensor MQTT/display bridge | `test_m1_display_mp`, `test_sensor_producers_mp`, `test_sensor_mp`; real producer/consumer schema3 metadata + snapshot, strict full-u32 counters, descriptor/unit/TTL/replay/reconnect/borrowed-buffer checks | protocol owner; ASan/UBSan PASS. `test_runtime_session_mp` preserves actual M1 ingress/session wiring, namespace/offline notifications/native grant dispatch |
| `test_runtime.c` account namespace/filter bounds/coalescing/stale generations, old MQTT receipts/preflight/first-cause tests | `test_runtime_mqtt_mp`, `test_native_mqtt` plus actual shared Paho lifecycle tests | shared native MQTT/Paho preflight/receipt/first-cause full old matrices; namespace/filter/HA rules additionally in `test_ha_runtime_contract` |
| `test_runtime.c` HA identity/names/optional entity cleanup; `test_mqtt_discovery_budget` | `test_runtime_mqtt_mp` fixed tiny JSON producer max-size, cursor/transport retry and actual parsed HA payloads; retain identity/layout/name/cursor/OOM cases | actual lifecycle/OOM/retry/wrap/transport recovery in migrated `test_mqtt_discovery_budget`; `test_ha_runtime_contract` adds full peer/group/name/tombstone and actual A1/M1/TC1 HA matrices |
| old `test_service{,_failure,_memory}.c`, HTTP JSON callbacks/owned/budget/background/status fixtures | `test_service_mp`, `test_http_product_mp`, native callback/fixed busy/MP stack/cache/status oracles | typed owner; service assertions migrated in `tests/service_mp/README.md`; callback/capture equivalence and residual ARM32 heap limits in `tests/http_core/NATIVE_COVERAGE.md` |
| backup/recovery/storage/pairing/auth fixtures | native backup MP `PHCBACK1` + actual persistent snapshot layout + auth/session cases | auth owner; backup, ordinary recovery, frozen layout, persistent pairing and passive grant/probe/session timing all remain native gates |
| Bemfa and product MQTT/HA/TC1 old fixtures | `test_bemfa_mp`, native TC1 business/HA + shared native MQTT | shared MQTT26/preflight4/birth3/buffer2/lifecycle1/Bemfa4 and TC1 stack3/checked-number2 retain original scenario assertions through MP/POD boundaries |
| `test_a1_fresh_air.py`: actual controller GPIO/PWM, sensor fallback, pulses/RPM, bounds/revisions, snapshot purity, PID saturation, independent coupling, shutdown | Same full assertions, build-time constant MP request vectors plus a direct writer for dynamic revision | features; ASan/UBSan PASS without JSON headers or library |
| `test_stock_runtime.py`: real M1 UART queue/buzzer/wake cadence/tones/music, OP16 details, OP20 alarm write/read/error retry, storage migration/rollback/CAS and physical reset ACK/retry/scope | Same full original scenario bodies, actual M1 main/uart/mcu/co2/display code and product MP dispatcher. Build-time request vectors and typed output observers link no JSON objects. Added four-busy stop and invalid-parameter preservation assertions. | features; ASan/UBSan PASS. Caught and fixed the wake-alarm-stop busy regression rather than dropping its assertion. |
| `test_m1_display_source`, `test_m1_display_format`, `test_m1_generic_fields`: configuration/UART/manual selection/dual lanes, rounding/bounds/lamps, remote generic fields/storage | All original 32 methods retained. Explicit test-only frozen JSON oracle builds MP at the test boundary; actual business parser consumes MP and producer writes MP. Schema2 inline fixture samples become schema3 metadata plus state. | protocol owner; complete original matrices, not replaced by short `test_m1_display_mp`. Invalid UTF8 metadata now rejects the entire metadata envelope; unknown descriptor is missing until metadata arrives. |
| `test_update.c`, `test_update.py`, `test_device_update_version.c` | Actual native deferred-worker full 16-row version matrix, source/admission/traversal and install count; actual local HTTP fetch302/chunked rejection for both ownership contracts | typed owner; `test_service_mp`, `test_release_manifest_mp`, `test_ota_fetch`; exact mapping in `tests/service_mp/README.md`. |
| `test_ota_recovery.py`: worker exiting/ready/OOM/reentrant recovery/throttling/clock wrap | Same original complete assertion body, extraction marker updated to `ota_job_status_mp` | features; ASan/UBSan PASS |
| `test_emw3080_release_memory`: 8KiB actual allocation floor, missing heap/overflow and all build profiles, distinct12KiB acceptance | Same original assertions, actual `ph_wire_malloc` replaces removed JSON allocator hook; actual MQTT permit retained | features; ASan/UBSan PASS |
| `test_info_metadata`, `test_capabilities_wire`: immutable metadata, model/resource/OTA action variants, envelope/OOM | Actual A1/M1 info/caps writer ten-variant matrix + native dispatcher envelopes/cache/unknown actions. TC1 actual SDK capture/product metadata and busy dispatch retained as independent native gates. Removed JSON fallback graph, full/live output contract and byte ordering are obsolete. | `test_product_metadata_mp`, `test_service_mp`; `test_typed_tc1_status`, `test_tc1_status_dispatch` and `test_tc1_status_guard` own TC1 source bounds/secret exclusion/metadata/reservation; no generic-capture substitution |
| `test_json_build_gate`, `test_service_json_failure_scope`, `test_http_json_owned`, `test_background_json`: independent ownership, allocation faults, foreground/background failure isolation and retries | Actual native service/HTTP callback owner-lifetime tests, writer OOM/foreign-thread isolation, Paho send generation/receipt and HA retry fixtures | Native writers have per-instance failure state and no global DOM lease. `test_http_product_mp`, `test_service_mp`, `test_mqtt_optional_json`, `test_native_service_mqtt`, `test_ha_json` run behavior; retired graph-lock internals are historical. |
| `test_status_bool`, `test_status_direct`: old full/live schema/typed fields/precision/lowheap | Frozen old schema oracle across all models, native full status precision/fault tests and actual capture purity/lock-race tests; native ARM modelled ownership/OOM job | `test_status_msgpack`, `test_status_views`, `--native-arm`. Full/live is intentionally replaced by full with event-invalidated cached bytes. ARM32 maximum full-runtime closed-SDK heap remains a separate measurement gap. |
| `test_memory_admission` fixed JSON busy ID parser | Native fixed busy/MP request ID ownership and callback503/noalloc cases; byte reserve/admission/wait scenarios remain directly executed | `test_http_product_mp`, `test_http_callbacks`, `test_service_mp`; old text busy parser is not a new product API |
| pure pinned JSON SDK allocation/parse/number/printbuf/ABI and three JSON-patch replay tests | Explicit historical SDK-only group, not a dependency of native product business fixtures | runner owner; `tests/run.sh --legacy-sdk` separates host SDK library tests; ARM/QEMU history uses `LEGACY_SDK_ARM=1` |

## Host vectors

`mp_fixture_vectors.py` uses Python's JSON parser only while generating constant
MP byte arrays before C compilation. The native automation test executable links
`typed_wire.c` and `wire_buffer.c`; it does not include/link JSON objects or a
runtime format bridge. Dynamic page/weekly requests use direct typed writers.

## Evidence boundary

Native compilation and sanitizer execution do not prove full firmware link,
DUT installation, concurrency P95, busy recovery, or physical-button acceptance.

## Executable entries (this branch)

- `SANITIZE=1 bash tests/run.sh --native-common`: 13 unittest cases; automation
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
- `ARM_CC=... bash tests/run.sh --all`: unified default native + retained full gate.
  It prepares all three pinned SDKs before the first product fixture; external
  checkouts can be provided through `A1_SDK_DIR`, `M1_SDK_DIR`, `TC1_SDK_DIR`,
  with `PHICOMM_M1_SDK`/`PHICOMM_TC1_SDK` inherited from those locations.
  `--historical-product` preserves the previous script, including old deleted-API
  implementation assertions; it is not a compatibility promise or release job.

The runtime controls test extracts the current production implementation on every
run, including `install_config`, applied-state/PID/manual/cap/animation functions
and `feature_mp.inc`. Historical assertion bodies live in
`runtime_control_scenarios.inc`; allocator, persistence, RTOS locks and group
transport are seams. The separate group/LAN modules own packet-level evidence.

## Remaining gates and evidence

- TC1 actual SDK capture/dispatch fixtures remain mandatory independent gates:
  pinned ARM32 plus host ASan tests validate field bounds, source freezing, busy
  reservation, secret exclusion and actual info/capabilities metadata. They are
  fully migrated; generic status stubs are not their replacement.
- Native ARM32 status and discovery jobs model the allocator as align8+8 and test
  ownership, OOM, pacing and recovery. They do not establish closed-SDK allocator
  fragmentation or the maximum concurrent full-runtime peak. That physical/target
  measurement remains separate from a passing host `--all`.
- Native compilation/sanitizers cannot establish firmware link, actual flashing,
  DUT P95/busy recovery or physical button acceptance.

Verified intermediate unified native manifest: **166 tests PASS, zero skipped**,
131.463 s (`/tmp/rc4-native-unified-final.log`). Later added full display/UART,
metadata, discovery and account matrices are undergoing the final unified run;
this count does not claim those later additions were part of the 166 run.

The prior native ARM entry passed on GCC5.4.1/newlib-nano plus qemu: A1/M1 retained
2824 B, modeled overlap peak7472 B; TC1 retained768 B, peak1992 B. OOM sweeps
preserve the first response and return to zero owned bytes. Log:
`/tmp/rc4-native-arm-entry.log`. These are bounded-fixture ledger values.

The explicit legacy SDK host entry passed 5 Python library tests + 3 C binaries,
with live=0 after10000 serial +4000 concurrent cycles. It is historical library
evidence, not a product JSON dependency (`/tmp/rc4-legacy-sdk-final.log`).
