# Native MP lane validation, 2026-10-28

No device flash, publication, signing or physical acceptance was performed here.
Source and packaging checks below are local build evidence only.

## Target artifacts

| Product | Exact clean input | ROM | RAM | OTA bytes | Staging limit | OTA margin | Pipeline |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| A1 full/default resources | c95051c9f637512afef83d0e1e72229ee98d551d | 251004 | 45604 | 730880 | 741376 | 10496 | exit 0 |
| A1 fresh-air/default resources | 6a6461caf4aba2ea55de7ef4f18defed612731c0 | 256700 | 45892 | 736576 | 741376 | 4800 | exit 0 |

The second input differs only by the ARM32 fixture and coverage document.
Both use pinned MiCO 3.6.4 commit
`6c465211d3ff8797cd835e400ec54a06530dd476`, ARM GCC 5.4.1, frontend
`6a92cc6ef7f8608937d3229d696ffb5f0f930679` (57203 B gzip), generated
candidate OTA metadata channel, and the unchanged production resource defaults.
Image fit, button/worker/HTTP/MQTT actual stack gates, no-JSON-object ELF gate,
and packaging all passed. `_printf_float`/newlib floating helpers can still be
linked; no claim of eliminating all float formatting code is made.

Local artifacts and logs:

- `build/a1-release/{manifest.json,SHA256SUMS,phicomm_a1.*}`;
  `/tmp/rc4-native-a1-final.log`.
- `build/a1-fresh-air-release/{manifest.json,SHA256SUMS,phicomm_a1-fresh-air.*}`;
  `/tmp/rc4-native-a1-final-fresh.log`.

A1 OTA SHA256:
`c1ba47d49c04383cdf0f737332b3108ae8d5545bb0633b0bd62c672839217036`.
Fresh-air OTA SHA256:
`44803216fc36c85c9083129a19698fb5f54bfa872182a4c7b9d77550f528b4de`.

Compared with earlier lane input cc4aedd, A1 full ROM increased 400 B and RAM
1544 B after the final shared auth/context, diagnostic and guard fixes. This
is an integration delta, not an attributed performance optimization result.
P95, busy frequency and busy recovery remain unmeasured for these images.

## Host and ARM32 execution

The following combined suite passed 43 tests with `SANITIZE=1`, 30.418 s;
log `/tmp/rc4-native-final-host.log`:

```
python3 -m unittest tests.test_service_mp tests.test_http_callbacks \
  tests.test_status_views tests.test_http_product_mp tests.test_status_msgpack \
  tests.test_release_manifest_mp tests.test_mqtt_ota_mp tests.test_version \
  tests.test_no_json_objects -q
```

`test_status_msgpack` uses a pinned SDK JSON implementation only as a test-only
frozen old-schema oracle. No JSON SDK sources are linked by the native route,
service, allocator, or target product paths.

A separate `tests.test_native_status_arm` passed with actual pinned ARM GCC
5.4.1/newlib-nano plus QEMU. Fixture ledger align8(request)+8, including
copy-before-free output growth: A1/M1 retained 2824 B / overlap peak 7472 B;
TC1 retained 768 B / overlap peak 1992 B. These are explicit zero-state fixture
watermarks, not maximum real-device heap measurements.

Detailed preserved contracts and remaining ARM concurrent virtual-runtime heap
coverage gap are recorded in `tests/http_core/NATIVE_COVERAGE.md` and
`tests/service_mp/README.md`. The old JSON implementation-specific fixtures
remain historical source, not passing native tests. The complete product CI
runner migration is owned by the feature/test lane and is not asserted here.

## Newly caught defects

- Actual OTA worker URL/model comparison incorrectly reused lowercase URL
  model against publisher uppercase identity; fixed separately.
- Candidate builds now select `/ota/candidate/<model>/<variant>/latest.msgpack`
  from generated provenance; stable builds use `/ota/v1/...`, with no fallback.
- 32-bit `strtol` saturation could admit out-of-range log arguments; reset and
  check `errno == ERANGE`, covered with ILP32 saturation emulation.
- Native wire reserve denial/OOM now have separate saturating diagnostics,
  preserving legacy JSON diagnostic fields for compatibility.

Actual metadata-worker fixtures execute service.c/update_mp.inc, then assert
publisher golden bytes, all three exact source URLs, one fetch/no fallback,
newer/equal/older version availability, signed-path-only install and borrowed
request destruction before deferred execution. Cryptographic signature/device
installation acceptance remains the existing signed-OTA validation boundary.
