# Status and diagnostics

`status.get`, `config.get`, and native state publications return complete readable business fields. Runtime memory, reset reason, MQTT errors, raw CO2 frames, and broker resource counters are excluded. CO2 calibration availability, cooldown and operation result remain business state under their existing readable paths.

`diagnostics.get` returns `{schema:1,fields:[[id,value],...]}` using the shared append-only `common/diagnostics-schema.json` registry. Unavailable measurements are omitted; false and zero remain present. Consumers must ignore unknown IDs and must not infer array positions from IDs.

The structured `ota_partitions` diagnostic value has `running`, `running_size`, `next`, `next_size` and `entries`. Each entry contains `label`, numeric `type`, `subtype`, `address`, and `size`. Missing running/next partitions use null labels and zero sizes. The array describes the actual installed partition table, not the build CSV.

The generated `src/diagnostics_ids.h` is produced by `tools/generate_diagnostics_schema.py --hub-root DAIKIN` in the canonical shared-schema checkout.
