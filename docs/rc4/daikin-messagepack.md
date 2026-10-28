# DAIKIN rc.4 management transport

DAIKIN full and hub retain ArduinoJson for HomeKit, external integrations,
configuration files, the local serial provisioning console and hub conversion
of existing JSON devices. This lane changes wire formats; it does not claim to
remove the document model or prove a runtime P95 improvement.

| Interface | rc.4 default |
| --- | --- |
| `/api/v1/info` | Public MessagePack identity/auth/wire discovery |
| `/api/v1/auth/login`, `/auth/logout` | MessagePack, device bearer sessions |
| `/api/v1/command`, `/status`, OTA result/errors | MessagePack only |
| `/api/*` hub account API | MessagePack wire, separate hub account sessions |
| Device MQTT command/result/state/capabilities/address/discovery | MessagePack |
| Sensor UDP read/probe and MQTT session grant | MessagePack |
| HA discovery and `/ha/state` | Independent JSON; measurement values only |
| Config backup/restore file | Existing versioned file format retained |

Device sessions have four slots with no eviction of live sessions, a 30-minute
idle lease and a 24-hour absolute lifetime. Login accepts `{password: string}`;
unset password still requires an automatic empty-password login, without a
password prompt. Changing the configured password invalidates device tokens.
A full table returns 503 `busy`; wrong/missing tokens return 401. Basic auth is
not accepted. JSON commands are not retried or reinterpreted as MessagePack.

Hub discovery explicitly selects old schema-1 JSON or canonical schema-2
MessagePack per account/model/MAC base. HomeKit and Bemfa commands, state and
receipts use that selected codec. Ordinary binary payloads are forwarded byte
for byte when account topics change: textual rewriting is reserved for HA
JSON payloads, where embedded account-qualified topics actually exist.

Sensor-v3 publishes retained `sensor/<id>/metadata` before nonretained complete
`/state` snapshots. Descriptor revision is the present-sensor bitset plus one,
so optional HCHO/PM10 field changes produce a new revision within a boot. The
metadata describes field names/types/units; snapshots carry value/valid/age,
boot, sequence and descriptor revision. Existing MQTT-authorized UDP grants
remain required. UDP password pairing is not implemented by this lane.

## Validation boundary

Host tests use the pinned production ArduinoJson and actual ESP32 WString for
native publication. This caught two binary hazards: ArduinoJson's default
Arduino String writer truncates at NUL, and ESP32 String `concat(p,n)` copies
`n+1` bytes. `wire_mp.h` therefore uses explicit lengths with padded scratch,
checks the complete byte count, and drops allocation-failed output.

Tests cover JSON/MP discovery, binary NUL, rejection of format fallback,
HomeKit acknowledged controls and confirmation queries, Bemfa commands and
receipts, UDP counters/expiry/wraparound, token capacity/logout/password change/
expiry, and existing OTA/HAP/restore/OOM behavior. Full and hub builds must both
fit their target partitions. No device flash or physical HAP acceptance is
claimed by these host/build checks. Embedded frontend remains the pinned rc.3
bundle until the frontend lane's lock update is integrated.
