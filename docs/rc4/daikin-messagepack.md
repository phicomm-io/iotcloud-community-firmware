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
The official IDF NVS + 16/20 KiB NOR host models also verifies capacity and
byte-by-byte interrupted writes. Physical device pairing and physical power-cut
acceptance remain untested. HAP reset spans another namespace and is not
made atomic with the management blob by this change.

## NVS host gate and rc.3 comparison

The NOR model tests both four-page (16 KiB) and five-page (20 KiB) NVS with
unchanged official IDF 4.4.7 sources at
`38eeba213aa695aabfd6d89aa9f5078dbe5a94c3`. The five compiled NVS translation
units and ROM CRC source were compared byte-for-byte with that official commit.
Flash reads, writes and erases are modeled; this is not a device power-cut test.
20 KiB is the current repository partition CSV, not an established physical
stock partition. The deployed DUT partition table remains unknown and must be
checked before device acceptance. WiFi occupies 512 bytes and a separate HAP
namespace occupies 2048 bytes in both capacity models.

| Capacity | Implementation / key count | Typical config, 12 real updates + reboots | Legal 6144 B config |
| --- | --- | --- | --- |
| 16 KiB | rc.3 `6950338`, no pairing feature | PASS, JSON/blob 3538 B | Existing capacity boundary: first update rejected; old config survives reboot |
| 16 KiB | rc.4 MP, 0 or 24 keys | PASS, JSON 3538 B / blob 3548 B | Admission rejects before any NOR write; old config/keys survive reboot |
| 20 KiB | rc.3 `6950338`, no pairing feature | PASS, JSON/blob 3538 B | Blob 6144 B; 12 real updates + reboots PASS |
| 20 KiB | rc.4 MP, 0 or 24 keys | PASS, JSON 3538 B / blob 3548 B | Blob 6177 B; 12 real updates + reboots PASS |

Each typical update changes the name and asserts additional NOR work; maximum
updates use distinct names too. The legal fixture includes 12 devices, eight
Bemfa mappings, eight bridge mappings, a 32-byte owner, 63-byte device name, and
valid topic lengths selected to reach the unchanged 6144-byte limit. No arbitrary
padding is used in this legal comparison. The fixed auth-store trailer is 780 B
whether zero or all 12 incoming plus 12 outgoing records are populated: filling
the records adds no further envelope length. MP config savings offset most of
that fixed cost in this fixture. No business limits were reduced.

The initial rc.4 JSON envelope was 6936 B for this 6144 B legal configuration;
its second update failed even with 20 KiB. The MP envelope fixes that regression.
A separate nonbusiness padding probe removes the format savings: hub's second
6144 B padding update is safely rejected in both capacities. Full's 3999 B
padding config with 24 keys is safely rejected on the second update at 16 KiB;
it sustains twelve updates at 20 KiB. These padding boundaries are preserved,
not reported as successful maximum business configurations.

Before a changed blob write, admission requires `ceil(blob_bytes/32) +
2*partition_pages + 1 + 126` free NVS entries: data, conservative chunk headers
and rounding, index, and a GC page. Global partition statistics include WiFi/HAP
and erased entries; this is a conservative admission check, not a guarantee
against concurrent writes or arbitrary fragmentation. IDF 4.4.7 can erase old
chunks while cleaning up a failed alternate-version multi-page write. Therefore
`putBytes != expected_length` immediately enters recovery, returns failure and
blocks HTTP/UDP authorization; it never reads the ambiguous generation back or
acknowledges success. Reboot loads and validates the actual persisted generation.
Successful writes still require exact readback. Capacity admission rejection
occurs before flash mutation and keeps current authorization usable.

For each capacity and each full/hub variant, every byte cut of first
password+key revocation (969 cases) and update (965 cases) reboots into matching
old config+keys or matching new config+empty keys: 7736 cases total. Failed NOR
writes, reboot and reset pass. Separate actual production-function fixtures
cover failed acknowledgements with old, new and corrupt persisted bytes;
failed acknowledgements always enter recovery, and no old credential is silently
restored from the legacy key when the authoritative envelope is invalid.
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
