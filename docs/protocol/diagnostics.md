# Shared on-demand diagnostics (schema 1)

`common/diagnostics-schema.json` is the single registry for A1, M1, TC1 and
DAIKIN. IDs are global, permanent and append-only: never renumber or reuse an
ID for another meaning, unit, or incompatible value type. Optional hardware and
model selection never change an ID. Removed fields retain reserved IDs.

`diagnostics.get` accepts an empty params map. Its normal command envelope has
`data: {schema: 1, fields: [[id, value], ...]}`. Each pair contains exactly one
unsigned field ID and its typed value. A field appears at most once. Arrays of
pairs intentionally avoid weakening the existing string-key command-map parser.
False, zero and empty text are values, not absence. Unsupported fields are
omitted, including the legacy `http_accept_queue_last_rc` placeholder. Consumers
ignore unknown IDs and reject duplicate IDs or an unsupported schema version.

The registry path describes the local display model, not a transmitted key.
For example the stable ID for `scd40.errors` is decoded into that nested display
object. Merge diagnostics into a separate view of the latest business status;
never replace business sensor objects with a partial diagnostic object or write
old diagnostics back into the current business state.

Regular `status.get` retains complete named business state (including error/
readiness flags required to operate the product, config-save errors, and live
OTA progress). Raw runtime/HTTP counters, watchdog details, panel/serial frames,
MQTT errors, OTA image verification metadata and optional sensor hardware
counters live in diagnostics. `uptime_seconds` remains a small operational clock
in business status for reboot detection and existing reset confirmation. Cached
business status is invalidated by authoritative events; its existing one-second
time boundary remains necessary for freshness and countdown fields. This is not
permission to cache valid sensor readings forever after samples stop.

Diagnostics are captured only on explicit requests and do not create a permanent
third cache. Ordinary A1/M1 captures no longer call runtime, watchdog or HTTP
snapshot getters; TC1 ordinary capture does not sample heap. Status/capability
cache lifetimes and independently owned transport responses remain separate.
Diagnostic reads are queries: no writes, receipt retention or business cache
invalidation. OTA may return busy rather than sampling additional state.

Generate all consumers from the registry:

```
python3 tools/generate_diagnostics_schema.py \
  --frontend-root /path/to/frontend \
  --hub-root /path/to/firmware/DAIKIN
```

Use `--check` in validation. Firmware `common/diagnostics_ids.h`, DAIKIN
`src/diagnostics_ids.h` and frontend `common/web/diagnostics-schema.js` are generated
artifacts, not independent schemas. The source schema is an offline build input;
devices do not generate, store or transmit a field-name dictionary at runtime.

Pressure-test diagnostics must be separately counted and bounded to pre/post
snapshots. Diagnostic polling must not become an extra high-frequency load used
to measure busy recovery. Missing counters are an observation gap, never zero.
