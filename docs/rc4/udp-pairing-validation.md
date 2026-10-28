# RC4 UDP pairing integration evidence

The local RC4 integration branch includes native pairing on A1, M1, TC1 and
Daikin. This is source, host-test and target-build evidence. No RC4 device
flash, live performance result or physical power-loss acceptance is claimed.

## Implemented behavior

- Empty or configured device password pairing exchanges an independent durable
  peer key for a scoped, boot-local UDP token. HTTP uses separate bearer sessions.
- Sender jobs own their inputs; successful responses must match the pending
  peer, IP, operation and binary nonce. Address changes invalidate the old token
  and start a fresh resume handshake at the new address.
- Removing or reducing one peer's permission revokes all of that peer's local
  sessions while preserving other peers and independent MQTT-issued grants.
- Successful durable unpair requests have four bounded 30-second receipts.
  Matching retransmissions do not repeat persistence. Incorrect peer, IP,
  nonce or token does not match; password/recovery epoch changes and reboot
  invalidate receipts. Failed persistence never creates a success receipt.
- Daikin uses the same LAN v3 binary sensor packets and declared integer units
  as MiCO. Both directions of pairing and lost-unpair-ACK recovery are exercised
  with production functions, not hand-written replacements for those functions.

## Storage and migration

Daikin commits configuration and incoming/outgoing durable peer tables in a
single NVS blob. Version 2 stores the configuration as MessagePack; version 1
JSON envelopes and legacy configuration remain readable. The existing JSON
backup/import format and logical configuration limits remain unchanged.
Uncertain persistence enters recovery and blocks authorization, including
already-queued HTTP contexts. A failed NVS write is not read back because the
IDF 4.4.7 partial-write cleanup can invalidate old chunks. Successful writes
still require exact readback. A capacity rejection occurs before any write and
preserves the live configuration and peer tables.

On the official IDF 4.4.7 NVS implementation with a 20 KiB NOR model, a valid
maximum 6144-byte hub configuration, Wi-Fi/HAP data and 24 directional keys
exposed a capacity regression in the initial JSON envelope: its 6936-byte blob
failed the second update. The MessagePack envelope is 6177 bytes and passed
three updates/reboots. The original configuration without pairing keys passed
the same three-update comparison. These are host NOR simulations, not reads
of a device's partition table.

The final capacity guard was checked on both 16 and 20 KiB NOR layouts, with
zero or 24 directional keys. Typical configurations passed 12 distinct writes
and reboots (each write increased flash work). At 16 KiB, the maximum legal hub
configuration is safely rejected without flash writes, matching the original
firmware's capacity boundary. At 20 KiB, the maximum passed 12 distinct writes
and reboots. Both variants' password revocation was also checked at 7736
interrupted-write cut points. These simulations do not establish the actual
partition layout or physical power-loss behavior of a device.

## Verification

- Integration source through `0b98efd`: 92 native regression tests passed, no
  skips. Subsequent local-input, metrics and watchdog coverage brings the
  canonical native set to 95 passing tests; the historical full gate still has
  migration work and is not represented by this subset.
- Integration `29ad292`: 7 focused Daikin pairing, auth and official NVS tests
  passed, including valid maximum configuration, failure/reboot/reset and
  per-byte interrupted writes for both variants. Nonempty incoming/outgoing
  key migration was then strengthened in `c3507ad` and retested successfully.
- Independent review reproduced the old multi-peer revocation and IP-change
  failures, then verified the fixes. The original Daikin-to-MiCO lost-ACK
  reproducer also passed after the shared receipt fix.
- Both Daikin full/hub builds and A1 full/fresh-air, M1 and TC1 builds passed
  their applicable image, stack and packaging checks after the shared fix.
  MiCO final ELF checks reject JSON object/parser libraries. These builds still
  use development version/frontend pins and are not release-ready RC4 images.
- Integration `76ec124`: 7 focused tests passed, including the strengthened
  official NVS dual-capacity comparison. Integration `3c11b8e` additionally
  rejects queued reset, reboot, state and special status operations after an
  uncertain commit, using the production queue dispatch branch in the test.

Reproduce the official NVS comparison after preparing pinned ArduinoJson and
the IDF dependency:

```sh
DAIKIN_NVS_IDF_SOURCE=/path/to/esp-idf-4.4.7 \
DAIKIN_NVS_RC3_COMPARE=1 \
python3 -m unittest DAIKIN.tests.test_lan_pairing \
  DAIKIN.tests.test_auth_storage DAIKIN.tests.test_web_auth \
  DAIKIN.tests.test_hub_config_storage -v
```

Before flashing: finish remaining regression migration, verify the target NVS
layout, freeze the exact frontend pin/version and rebuild the candidate, then
verify identity, backups and rollback artifacts for each device.
