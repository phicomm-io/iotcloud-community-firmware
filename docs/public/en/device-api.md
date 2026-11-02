# Device interfaces and protocols

This guide uses **v1.0.0-rc.5 (Beta)** and **v0.9.27 (stable)** as integration baselines. Check the model, running version and variant in the device WebUI before choosing an encoding and capabilities. The M1 sensor controller has its own version.

## Baselines and encoding

| Item | RC5 | 0.9.27 |
| --- | --- | --- |
| Devices | TC1, M1, A1, DAIKIN, ORVIBO S30-V1B | TC1, M1, A1, DAIKIN |
| Ordinary HTTP commands | MessagePack, `application/msgpack` | JSON, `application/json` |
| MQTT device business | MessagePack, TMCP v2 envelopes | JSON; TC1 supports Legacy v1 or TMCP v2, others use TMCP v2 |
| Sensor fields | Schema 3, separate descriptors and live snapshots | Use that release's sensor / automation interfaces; do not apply schema 3 |
| M1 user display configuration | `display_profile.get/set`, GET schema 4 | Use its CO₂ display and controller capabilities; no profile4 |
| Home Assistant | Separate JSON discovery / state / control topics | Separate JSON discovery / state / control topics |

The project name is `iotcloud`, the device protocol is `openiot`, and TMCP identifies the internal command envelope. HTTP `/api/v1`, TMCP `protocol_version:2`, binary `wire_version:1`, sensor schemas and MCU UART versions are independent. Here, proto means protocol definitions; device interfaces do not use Protobuf `.proto` files.

S30-V1B also advertises CBOR support; these cross-device examples use the default MessagePack encoding.

RC5 does not downgrade after a JSON request, timeout or `busy`. MQTT `availability` remains plain text `online` / `offline`; firmware uploads and configuration backups retain their own binary file formats.

## HTTP access

Without a management password, access is open and creates no login session. With a password, Basic authentication accepts username `admin` and the device Web password. MQTT Broker credentials and UDP pairing credentials are managed separately. Use HTTP on a trusted LAN. Browser access across origins also requires the device's Origin / OPTIONS checks and browser local network permission.

Common RC5 routes follow. Ordinary GET queries return data objects; POST commands return envelopes with `code/data`. Discover actions through `capabilities.get` → `data.actions`. Specialized routes also depend on model and variant.

| Method and path | Result or purpose |
| --- | --- |
| `GET /api/v1/info` | Model, device ID, firmware version and encoding markers |
| `GET /api/v1/status` | Device state snapshot |
| `GET /api/v1/capabilities` | Actions, capacities and optional features |
| `GET /api/v1/config` | Redacted settings without passwords |
| `POST /api/v1/command` | Queries, controls and configuration commands |
| `GET /api/v1/memory` | Memory and transport diagnostics, where implemented |
| `GET /api/v1/config/backup` | Complete binary backup; requires a configured management password; protect the included credentials |
| `POST /api/v1/config/restore` | Restore a matching model / layout backup; reboot after acceptance |
| `POST /api/v1/ota/upload` | Signed file stream on TC1 / M1 / A1 / DAIKIN |
| `POST /api/v1/mcu/exchange` | M1 raw MCU frame bridge, separate from MessagePack commands |
| `POST /api/v1/ota/convert` | Capable DAIKIN `full → hub` conversion; follow the [hub guide](daikin-guide#switch-from-full-to-hub) |

S30-V1B uses `ota.start` with its `.rbl` URL. It has no local file upload, configuration backup / restore or MCU bridge. See [firmware updates](firmware-update) for package selection and upload steps on the other devices.

These dictionaries show the decoded structure. RC5 transmits MessagePack bytes:

```python
request = {"request_id": "read_001", "action": "status.get", "params": {}}
response = {"request_id": "read_001", "code": "ok", "message": "Operation successful", "data": {}}
```

Use `code` to classify results: `ok` for a successful query and `accepted` for an accepted operation. Handle `invalid_params`, `unsupported`, `busy`, `conflict` and `storage_error` separately. `message` is display text. Give writes unique request IDs and never change the content under an existing ID. Receipt deduplication uses a bounded device cache. After a timeout, query actual state before deciding what to do; do not automatically replay switching, reset or upgrade commands. HTTP overload can return JSON `busy` or close the connection before an HTTP response exists.

## Python query example

Download the [query script](assets/examples/device_query.py). It only queries capabilities, status and redacted configuration, never retries automatically, and prompts for passwords. RC5 needs Python's `msgpack` package; 0.9.27 uses the standard library.

```sh
python3 device_query.py http://192.168.1.100 --baseline rc5
python3 device_query.py http://192.168.1.100 --baseline rc5 --action status.get --password
python3 device_query.py http://192.168.1.100 --baseline 0.9.27 --action status.get
```

The script checks HTTP status, response encoding, result code and request ID. Its printed JSON is for terminal display.

In a Python environment with `msgpack`, create a request file for curl:

```sh
python3 - <<'PY'
from pathlib import Path
import msgpack
request = {"request_id": "caps_001", "action": "capabilities.get", "params": {}}
Path("request.msgpack").write_bytes(msgpack.packb(request, use_bin_type=True))
PY
curl --fail-with-body --max-time 10 \
  -H 'Content-Type: application/msgpack' -H 'Accept: application/msgpack' \
  --data-binary @request.msgpack http://192.168.1.100/api/v1/command -o response.msgpack
python3 - <<'PY'
from pathlib import Path
import msgpack
print(msgpack.unpackb(Path("response.msgpack").read_bytes(), raw=False))
PY
```

If a password is configured, add curl's `--user admin` and enter it at the prompt. For 0.9.27, send JSON directly:

```sh
curl --fail-with-body --max-time 10 -H 'Content-Type: application/json' \
  --data '{"request_id":"caps_001","action":"capabilities.get","params":{}}' \
  http://192.168.1.100/api/v1/command
```

## MQTT commands and discovery

The RC5 device root is `phicomm/v2/<scope>/<model>/<device_id>`. Scope is `u_` plus lowercase hexadecimal UTF-8 username bytes, or `local` without a username. Device IDs are factory MAC addresses as 12 lowercase hexadecimal digits. Read the model segment from discovery, such as `tc1`, `m1`, `a1`, `daikin` or `s30-v1b`; do not infer it from the chip.

| Topic | Retain | RC5 payload |
| --- | --- | --- |
| `phicomm/v2/<scope>/discovery/<device_id>` | Yes | MessagePack schema 2 announcement with `wire_version:1`, `wire_formats:["msgpack"]`, `default_wire_format:"msgpack"` |
| `<root>/availability` | Yes / LWT | Text `online` / `offline` |
| `<root>/capabilities`, `<root>/state` | Yes | MessagePack capabilities / state |
| `<root>/command` | No | Same MessagePack request as HTTP |
| `<root>/result` | No | MessagePack receipt matched by `request_id` |

Subscribe to results before publishing a command to capture fast replies. This example only queries capabilities; add authentication and TLS options required by your Broker:

```sh
mosquitto_sub -h BROKER -t 'phicomm/v2/local/tc1/001122334455/result' -C 1 -N > result.msgpack
# In another terminal, send the request file created above; do not add -r.
mosquitto_pub -h BROKER -t 'phicomm/v2/local/tc1/001122334455/command' -f request.msgpack
```

Home Assistant uses `homeassistant/.../config` and the device's HA JSON topics. Do not pass ordinary MessagePack results to HA JSON templates. Configure Broker account ACLs; topic prefixes alone do not isolate accounts.

## Sensor field protocol

RC5 schema 3 uses two MessagePack topics:

- `phicomm/v2/<scope>/sensor/<device_id>/metadata`: retained field descriptors.
- `phicomm/v2/<scope>/sensor/<device_id>/state`: non-retained complete live snapshots.

```python
metadata = {
    "schema": 3, "device_id": "001122334455", "boot": 123,
    "descriptor_revision": 1,
    "fields": {"power": {"name": "Power", "type": "number", "unit": "W"}},
}
state = {
    "schema": 3, "device_id": "001122334455", "boot": 123,
    "descriptor_revision": 1, "sequence": 7,
    "sensors": {"power": {"valid": True, "value": 12.5, "age_ms": 0}},
}
# Encode each separately with msgpack.packb(..., use_bin_type=True).
```

`boot`, `descriptor_revision` and `sequence` are nonzero uint32 values. Device identity, boot and descriptor revision must match between metadata and snapshots. Bind `device_id + field_id + type + unit`; names are labels. New snapshots revoke old values for missing or invalid fields. Invalid readings use `valid:false/value:null`, never a fabricated zero. Duplicate, out-of-order or retained state and unknown descriptors cannot renew freshness. Source age plus elapsed local time must satisfy both field freshness and display TTL: the generic ceiling is 20 seconds; some A1 environment fields use 15 seconds. M1's three-digit glass, display format and controller capabilities impose further value limits.

## UDP and MCU proto

| Protocol | RC5 definition | Source |
| --- | --- | --- |
| Sensor LAN v3 | MessagePack `read/probe/sample/pong`, 16-byte bin token, integer measurements in fixed units | `common/lan_wire.h`, `common/lan_wire.inc` |
| Business LAN v4 | UDP 11288, `_openiot._udp.local.`, `command/result/pull/ack`; inner command is a MessagePack bin | `common/lan_wire_v4.h`, `common/lan_wire_v4.inc` |
| M1 UART | Raw serial frames, opcodes and capability negotiation; `/mcu/exchange` bridges frames | `M1/protocol.h`, [controller update guide](m1-mcu-update) |

LAN v4 limits are 1200 bytes per frame, 1024 bytes per command body and 8192 bytes per response. Responses use 768-byte chunks and a five-second cache. Tokens are 16 binary bytes, not hexadecimal strings. Obtain them through pairing / session authorization; an empty management password does not bypass UDP authentication. Use pull for missing chunks and ack on completion; do not execute a write again to retrieve its result.

## Implementation and specification sources

RC5 statements were checked against [fixed source c09a067a](https://github.com/phicomm-io/iotcloud-community-firmware/tree/c09a067afe577c721d49c113e0c8ce88afb1f951): `common/service_mp.inc` (commands), `common/service_product_mp.inc` (capabilities / identity), `common/mqtt/discovery_mp.h` (discovery), `common/sensor_mp.h` (schema 3) and `common/lan_wire*.inc` (UDP). For 0.9.27, use the [stable source](https://github.com/phicomm-io/phicomm-community-firmware/tree/v0.9.27).

The [developer protocol repository](https://github.com/phicomm-io/iotcloud-community-protocols) provides contract indexes, schemas, vectors and generated types. `protocols.lock.json` pins a consumed revision; its historical JSON contracts do not replace the current RC5 binary implementation. Interface changes must update protocol definitions, positive / negative examples, generated artifacts and consumer locks together.
