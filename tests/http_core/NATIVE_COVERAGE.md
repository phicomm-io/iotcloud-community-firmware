# Native MessagePack regression coverage

Product callbacks in `test_http_callbacks` are extracted unchanged from A1,
M1 and TC1, with only platform/config/flash/service and auth boundaries replaced.
The real parser, scheduler, shared product owner, codec and MQTT diagnostics run
under ASan/UBSan. Each A1 (3 slots), M1 (4), TC1 (3/4) case executes 300 mixed
read-only cycles by default. `PH_HTTP_CALLBACK_SOAK_ROUNDS` selects 1..100000.
This is host lifecycle evidence, not Wi-Fi latency, target heap or DUT evidence.

| Old contract | Current executable coverage |
| --- | --- |
| HTTP owned output, allocation refusal, waiting/retry/release | `test_http_product_mp` and `test_http_callbacks` |
| Slow/truncated command bodies, synchronous borrowed input, independent response | same; callback scheduler receives explicit binary byte length including NUL |
| Raw PHCB backup/restore size, password guard, failure codes, interruption | `test_http_callbacks` (all 4 model/slot combinations) and auth/storage backup fixtures |
| OTA admission before init, write failure, interrupted upload, unsigned recovery | callback fixture; TC1 additionally response delivery before activation, send/close failure and uncertain commit |
| MCU framed exchange and upload | M1 actual callback fixture: MP bin wrapper, binary NUL, response bin, length/truncation, timeout, invalid vectors, abort and commit |
| Full status schema and primitive precision | `test_status_msgpack`: frozen 6950338 visitor oracle across 5 variant/feature combinations, all 3 models, boundary values, deterministic/fixed/measure output, allocation refusal |
| Frozen capture purity, state mutation, lock race, sensor stale/null | `test_status_views`: actual A1/M1 capture and native encoder, no getter/lock/allocation while capture locks held, 10000 full encode cycles, M1 update injected while waiting for locks |
| Old full/live projection | superseded by complete-full wire contract; capture-without-diagnostics remains tested, transport accepts full/live compatibility alias but returns full cache (`test_service_mp`) |
| Background HA/virtual discovery lifetime and retry | `test_runtime_mqtt_mp`, `test_ha_json`, `test_sensor_producers_mp`: actual producers, separate HA namespace, escape limits and retry, no normal JSON bridge |
| Token lifecycle | `test_auth_routes_mp` and `test_local_auth`; callback test uses an explicitly revocable Bearer boundary to exercise recheck during body upload |

The old `test_http_json_budget` ARM32 graph overlap/peak allocator tests depend on
removed JSON graph implementations. Host MP ownership/pressure tests and target
stack/ELF gates replace their failure-path coverage but do **not** establish an
equivalent ARM32 concurrent peak-heap watermark. That measurement remains an
explicit validation gap; legacy source fixtures are retained, not silently
claimed as passing native tests. Historical JSON primitive-sharing and count/fill
implementation tests likewise cannot run against functions intentionally removed.
