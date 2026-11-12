# Authenticated LAN business v4

The MXCHIP implementation shares `common/lan_wire_v4.h/.inc` with the DAIKIN
hub/client. This extends the existing native MessagePack sensor channel without
removing v3 READ/PROBE/SAMPLE/PONG or its SENSOR grants. It is a local business
transport; upstream MQTT/Bemfa and external Home Assistant brokers are separate.

## Discovery and authorization

Advertise `_openiot._udp.local.` on the Station interface only, SRV port 11288.
TXT explicitly contains `lan_wire=4`, `version=4`, `codec=msgpack`, lowercase
12-hex `id`, `model`, `variant`, `firmware`, and `udpport=11288`. Identity comes
from compiled product metadata. Discovery does not authenticate a peer.

Existing password pairing returns a durable per-peer key. `auth_resume` exchanges
it for a boot-local bearer token. Business clients add a nonzero uint32 `boot`:
a changed boot rotates the token/context; an identical boot retries the live token
without extending idle/max TTL or writing Flash. The old optional-field-free
sensor path is preserved. Possession of the durable key authorizes a fresh resume;
replaying an old resume cannot recover its former token, but this is not an
anti-replay or encrypted replacement for the existing pairing transport.

Only valid UDP_PAIR or UDP_MQTT contexts with CONTROL scope can use v4. SENSOR
alone never grants complete status or control. Service validates the context a
second time inside the same command critical section used by ordinary callers.
A stale context cannot replay a receipt after revocation.

## Wire contract and bounds

All datagrams are MessagePack maps, maximum 1200 bytes. Required common members:
`v:4`, `src`, `dst`, nonzero uint32 `boot`, nonzero uint32 `id`, `op`, and a
16-byte bin `token`. IDs are lowercase 12-hex device identities. Unknown/duplicate
outer fields, invalid UTF-8/types and trailing bytes fail strict parsing.

- `op:"command"`: `body` is a bin containing one complete TMCP MessagePack
  `{request_id,action,params}` request, 1–1024 bytes. There is no inbound request
  fragmentation or unbounded reassembly. Oversize/malformed frames are dropped.
- `op:"pull"`: zero-based uint `part` requests a response fragment.
- `op:"ack"`: releases the retained response. A duplicate current ACK is harmless.
- `op:"result"`: `boot` is the responder boot and `request_boot` echoes the
  requester's boot. `code:"ok"` includes `part`, `total`, `size`, `body` bin.
  All other codes omit those four members. Codes are `busy`, `expired`,
  `conflict`, `too_large`, `auth_required`, `invalid_params`, and `acked`.

The complete response is at most 8192 bytes. Fragments are exactly 768 bytes,
except the final shorter fragment. First command reply returns part 0; the client
pulls the remainder and checks identity/token/boots/id/part/total/size. One owner
retains exactly one independently owned response for a fixed, non-renewable five
seconds. Pull/retry cannot extend expiry. ACK immediately permits another owner.
There is no second full response or full-response transport copy. A new request
from the same owner releases its previous bytes before construction.

The server retains four context-bound digest/high-water records. Same id and body
returns the retained first fragment; same id and different body is `conflict`.
An older/ACKed/expired admitted id is `expired`, never re-executed. A definite
pre-dispatch admission failure returns `busy` and does not consume the id. If a
request was admitted but the output is lost/OOM, the id is burned; retry is
`expired`, since execution may already have happened. Send failure retains the
owned response and high-water record. Revocation/expiry invalidates its context.

The client keeps one pending operation per device, a stable boot and strictly
increasing id, and retries identical bytes/id only. It must not convert timeout
into a new id/codec write. ID exhaustion requires a new boot/token session.
The transport does not promise exactly-once execution across device power loss.

## Service and ownership

`service_lan_command_mp(data,length,context)` and the admitted-aware variant use
the existing typed dispatcher. The explicit CONTROL allowlist includes complete
status/capabilities/info/diagnostics, existing product reads, and individual
fan/light/lock/outlet/indicator/display/buzzer/wake-alarm/fresh-air controls.
Fresh-air config/linkage reads are allowed; their setters remain denied.
Generic config, Wi-Fi, OTA, reset, password, pairing, schedules and sensor-session
administration are denied. Unsupported per-product operations remain unsupported.

Wire request_id is preserved. Service receipt keys derive from peer/source/slot/
generation and request_id, with an additional LAN source discriminator separating
them from HTTP/MQTT receipts. The raw request digest still detects conflicts.

Views are borrowed only until synchronous dispatch returns. Retained async product
jobs must own validated copies, as on existing HTTP/MQTT entry points. The output
is owned independently before the worker reuses input as fixed fragment scratch.
No callback may retain the input pointer or re-enter the serialized server.

A1/M1 use their existing binding worker. A1 polls local buttons before/after LAN
handling and uses immediate semaphore admission instead of waiting for another
command owner. Existing command/control/cache-eviction mutex users share that
semaphore. TC1 has a lower-priority UDP worker with a fixed 12288-byte stack and a
1201-byte guarded IO allocation. OTA pauses/reclaims the LAN response/socket/IO;
failed or cancelled OTA retains exclusion until the replacement worker is ready.

## Validation boundary

Host tests cover the actual codec, server, auth, sender, dispatcher, mDNS SDK
functions and OTA lifecycle. `tools/check_lan_stack.py` checks actual product
`.su` plus linked symbols for the complete worker→server→service→status,
diagnostics/capabilities, auth, hash and fragment tails. Its 12288-byte task budget
and 2048-byte SDK/crypto reserve are fixed; missing/nonstatic frames fail closed.
This static bound is not a physical RTOS watermark or local-button latency test.
Firmware flash/RAM/stack acceptance requires the exact six-grid target builds;
DUT discovery, pairing/control, broker-loss recovery and buttons remain separate
physical acceptance tasks.
