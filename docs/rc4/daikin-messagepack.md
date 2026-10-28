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
boot, sequence and descriptor revision. Native LAN uses the shared v3 typed packet codec (binary 16-byte token,
uint32 counters, fixed milli-Celsius/milli-percent/milli-PM25, micro-HCHO and
integer CO2 units). MQTT temporary grants remain separate from local pairing.

Local UDP pairing supports empty or configured device passwords, durable
incoming and outgoing keys, explicit resume to a boot-local token, and
`lan.pair`, `lan.pair.get`, `lan.unpair` management actions. Sender challenges
bind peer, source IP, operation, nonce and credential epoch; asynchronous jobs
own their password/key/token data. A changed trusted peer IP starts a new
challenge. A successful unpair reply can be repeated for 30 seconds only for
the same peer/IP/nonce/token/epoch, so a lost acknowledgement does not make a
successful revoke appear to fail. Removing an outgoing key follows a peer ACK.

Configuration and both pairing directions share a versioned atomic NVS blob.
A failed acknowledgement is resolved by exact readback: new means committed,
old means unchanged, and any unknown state enters recovery and blocks HTTP and
UDP authorization. Password change, reset and restore revoke local device
sessions and both key stores; MQTT grants remain independent. Backups export
configuration and routing addresses only, never pairing secrets. Legacy config
keys are left untouched, so flashing older firmware can restore old credentials;
this is a rollback boundary, not a supported credential-revocation mechanism.

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

The follow-up host fixtures execute actual production persistence functions
with injected old/new/unknown outcomes, reboot, force-revoke and failed-save
rollback for full and hub. The LAN fixture calls the actual MiCO encoder,
DAIKIN pollSessions, shared decoder, sender and receiver. It checks unsigned
counter boundaries, binary tokens, fixed units, invalid fields, empty/configured
passwords, key-vs-token distinction, durable resume, outgoing unpair and lost ACK.
The official IDF NVS + stock-size NOR host model also verifies capacity and
byte-by-byte interrupted writes. Physical device pairing and physical power-cut
acceptance remain untested. HAP reset spans another namespace and is not
made atomic with the management blob by this change.

## NVS host gate and rc.3 comparison

The NOR model uses the repository partition CSV’s 20 KiB NVS size and official IDF 4.4.7 sources at
`38eeba213aa695aabfd6d89aa9f5078dbe5a94c3`. The five compiled NVS translation
units and ROM CRC source were compared byte-for-byte with that official commit.
Flash reads, writes and erases are modeled; this is not a device power-cut test.
This does not establish a deployed device’s partition table: a retained older
16 KiB layout would need its own capacity acceptance.
WiFi occupies 512 bytes and a separate HAP namespace occupies 2048 bytes.

| Implementation | Legal JSON-equivalent config | Pair records | Durable blob | Three updates + reboots |
| --- | ---: | ---: | ---: | --- |
| rc.3 writer from `6950338` | 6144 B | 0 (feature absent) | 6144 B | PASS |
| Initial rc.4 JSON envelope | 6144 B | 12 incoming + 12 outgoing | 6936 B | Second update safely rejected |
| rc.4 MP envelope | 6144 B | 12 incoming + 12 outgoing | 6177 B | PASS |

The legal fixture includes 12 devices, eight Bemfa mappings, eight bridge
mappings, a 32-byte owner, 63-byte device name, and valid topic lengths selected
to reach the unchanged 6144-byte limit. No arbitrary padding is used in this
legal comparison. Full also sustains twelve updates at its 3999-byte writer
limit with both full key tables and the same WiFi/HAP pressure.

A separate nonbusiness padding probe deliberately removes the format savings:
hub's second 6144-byte padding update is rejected and the previous config and
keys remain readable. This failure is retained as a capacity boundary, not
reported as a successful maximum business configuration.

For full and hub, every byte cut of first password+key revocation (969 cases)
and update (965 cases) reboots into matching old config+keys or matching new
config+empty keys: 3868 cases total. Failed writes, reboot and reset also pass.
The envelope now stores MP (version 2) while reading prior version-1 JSON
blobs; exported backup files remain JSON. Fresh `{}` configurations are tested
after `shrinkToFit`, so the first pair can grow `_version` safely.

Reproduce the optional NOR suite with official source prepared locally:

```sh
DAIKIN_NVS_IDF_SOURCE=/path/to/esp-idf-4.4.7 \
DAIKIN_NVS_RC3_COMPARE=1 \
python3 -m unittest DAIKIN.tests.test_hub_config_storage -v
```

The rc.3 comparison option requires commit `6950338` in the local Git history.
Without the optional IDF source, this suite reports SKIP, not PASS.
