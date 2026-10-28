# Frozen status MessagePack output

`device_status_mp(writer, snapshot, full)` emits exactly one root status payload
from the caller's frozen snapshot. `full` is required. The function neither
reads device state nor adds a transport envelope; invalid capture masks, model
mismatches or failed output mark the writer failed. To nest it, call
`phw_root_key(writer, "data")` in an already open response map before encoding.
A caller can cache the resulting `ph_buf` and use `phw_cached_value` to return it
without traversing the snapshot again. Cache invalidation and snapshot capture
remain the business owner's responsibilities.

`device_runtime_mp` emits runtime members into an already open map. It does not
emit a standalone root; this allows capability/runtime composition by callers.

The frozen visitor retains the existing fields, conditional availability,
password-set booleans, device-specific diagnostics and capability branches.
Integers retain the existing signed-field behavior. Numeric fields are emitted
as binary64 without the former JSON six-significant-digit decimal rounding.
M1 last-frame and A1 panel-raw hexadecimal strings remain lowercase with the
same widths, now emitted without snprintf.

The previous graph and JSON-wire entry points (`device_status_encode`,
`device_runtime_encode`, `device_status_wire`) are removed. HTTP/MQTT and model
adapters must migrate their callers; live/full compatibility decisions belong
to those adapters. `firmware_variant_defs.h` is a pure constant header.
`firmware_variant.h` temporarily remains an explicit legacy JSON adapter for
unmigrated callers; status compilation includes only the constant header.

`tests.test_status_msgpack` compares decoded binary output against a test-only
copy of the exact baseline visitor/sink at firmware commit
`69503385676ba40217e248cfb429e09b712ac580`. It includes the baseline additive
sensor/filter fields, not an older partial schema. The production status
translation unit links only writer functions; the JSON implementation exists
solely in the test oracle. All emitted keys and map/array sizes are checked.
Integers compare exactly; floating fields allow the old JSON wire's explicit
six-significant-digit rounding bound. The tests cover A1/M1/TC1 across five
resource/fresh-air/Bemfa combinations, stale/missing/nonfinite sensor values,
UTF-8 and escaped text, frozen-output determinism, envelope embedding,
measurement/fixed output without allocations and refusal cleanup across growth
points. Run with `SANITIZE=1` for ASan/UBSan.

The oracle uses the pinned MiCO JSON source and production numeric adapter.
With `M1_SDK_DIR` pointing to a pinned SDK Git checkout, it extracts the pinned
Git objects into temporary storage before applying the current overlays. This
avoids unrelated SDK working-tree instrumentation changing the oracle build.
CI may instead supply its already prepared `PHICOMM_TEST_JSON_DIR`. No SDK
working files are modified. Host parity does not establish target stack, flash
size, performance or device acceptance; product build and live tests remain
separate gates.
