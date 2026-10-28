# rc.4 MessagePack integration

Status: implementation in progress. No rc.4 firmware has been flashed or released.

## Source and release identity

Release repository: `phicomm-io/iotcloud-community-firmware`.
Integration starts at main `69503385676ba40217e248cfb429e09b712ac580`;
the published rc.3 source is `02de2b8babd22235136a09eea277fa5b3251c9bf`.
Preserve rc.3 recovery, generic sensor binding, display profile4 and existing
features. Experimental old-repository PRs are porting inputs, not the release base.
Target is `v1.0.0-rc.4`, candidate channel only; stable must not move.

## Accepted architecture

- Device structured HTTP, cloud MQTT and UDP use MessagePack only. Remove
  JSON/CBOR management compatibility and JSON DOM business bridges on MiCO.
- Borrow request bytes only during synchronous processing; asynchronous work
  owns validated typed data. Use bounded direct output and explicit byte lengths.
- HA retains an independent bounded JSON writer and necessary small input parser.
  Daikin retains ArduinoJson for external protocols and old-device conversion.
- Cloud/frontend discover identity, firmware and supported wire profile, then
  select old JSON or new MP. Never retry writes using another codec after failure.
- Keep existing binary persistence and PHCBACK1. Preserve validation and recovery.
- Publish matching latest.json/latest.msgpack; new firmware consumes MP.
- Include event-invalidated full status/capabilities caches. Generate request IDs,
  authentication and dynamic age/uptime/TTL per response; no stale generations.
- Local token auth covers all frontend entrypoints. UDP local pairing and
  MQTT-issued temporary authorization remain independent. Direct-link peers
  must upgrade together; no legacy JSON UDP converter is in scope.

## Remote display

Introduce sensor-values-v3: retained `sensor/<id>/metadata` descriptors identified
by boot and descriptor revision; non-retained complete state snapshots carry
boot, sequence, descriptor revision and value/valid/age_ms per field.
Preserve arbitrary field identity/type/unit binding and profile4 configuration.
Missing, invalid, unsupported or unit-mismatched fields revoke old numeric values;
unknown descriptors do not renew leases. Preserve replay protection, two lanes,
TTL, rounding/saturation, thresholds, switching and MCU confirmation.
Do not change UART operations or flash the sensor MCU for this migration.

## Work ownership

Each lane works in an isolated branch/worktree based on the integration source.

| Lane | Responsibility |
| --- | --- |
| native-mp | Shared core, service/product dispatch, A1/M1 HTTP, OTA consumption, caches |
| features | Configuration codec, automation, typed feature business operations |
| tc1-ha | TC1 full message activation, HA writer/parser and numeric formatting |
| auth-udp | HTTP sessions, local pairing, typed UDP/group transport |
| protocol-display | Shared contracts/code generation, M1 remote sources and sensor-v3 |
| cloud | Discovery, device codec selection, binary relay and metadata ACLs |
| frontend | Unified transport, token coverage, source selector and resource tests |
| daikin | Full/hub MP transport and old-device conversion |
| release-metadata | Canonical dual metadata, catalogs, archive/hash verification |
| root | Integration, dependency review, release locks and acceptance |

Shared runtime/engine edits require coordination: UDP and feature handlers must
not independently replace the same functions. Do not overwrite unrelated work.

## Acceptance

Build all ten release variants and run cross-language vectors, malformed input,
OOM, ASan/UBSan, actual stack/ELF and full business coverage checks. Verify
authentication lifecycle, backup/recovery, OTA signatures and local buttons.

Live targets: A1 .111, M1 .103, TC1 .120, Daikin .110 on 10.117.152.0/24.
Freeze identities, backups and exact rollback artifacts before writes.
Compare rc.3, MP without cache and MP with cache, three 600-second rounds each.
Keep HA, MQTT user, HTTP home and heavy settings concurrent, with three tab
switches within one second every thirty seconds. Separate cancellations,
superseded work, pending operations, network failures and application failures.

Per round: raw busy <0.1%, no visible page failure, successful operation P95 <=2s,
no increase in timeouts. Recovery scenarios probe read-only at 250ms, targeting
P95 <=2s and maximum <=5s; record unrecovered events separately. Do not inflate
queues/retries to conceal overload. Record build, transfer, runtime identity,
performance and physical acceptance independently. No experimental PB, CBOR,
numeric-key format, lazy MQTT or compiler change enters this release by default.
