# rc.4 hub LAN architecture and acceptance

This document specifies the replacement architecture and acceptance requirements.
It is not a record of completed hardware tests. The older broker-based simulation
report does not establish acceptance of this architecture.

## Transport contract

- The hub runs no embedded MQTT broker, TCP MQTT listener, or MQTT-over-WebSocket
  listener. Local devices communicate with it through authenticated UDP.
- Discovery uses `_openiot._udp` with explicit device identity, model, variant,
  firmware, UDP port, `lan_wire=4`, and `codec=msgpack`. Discovery supplies routing
  hints; it never grants permission or automatically registers a device.
- WebUI owner authentication protects registration, password pairing, revocation,
  and route confirmation. An unauthenticated mDNS address change cannot redirect
  a stored pairing secret. CONTROL permission does not authorize configuration,
  OTA, reset, or password administration.
- LAN v4 has bounded requests and responses, pull-based response chunks, and
  authenticated transaction identity. Retries preserve the original request ID
  and command bytes. A lost response must not execute a write twice.
- Upstream MQTT is an external client connection. Bemfa and HomeKit adapters
  consume the same local device bus. Local Home Assistant MQTT integration
  requires an external broker; the hub does not substitute a local broker.
- Existing cloud topic/discovery identities are not renamed merely because the
  LAN discovery service changed. Cloud compatibility with old JSON devices and
  new binary devices remains a separate platform acceptance requirement.
- Ordinary business status uses readable fields. Numbered diagnostics are read
  separately with `diagnostics.get`, schema 1. Unsupported fields are omitted;
  consumers skip unknown diagnostic IDs.

## Required test matrix

| Area | Scenarios | Required evidence |
| --- | --- | --- |
| No embedded broker | Boot in hub and non-hub modes; reconnect Wi-Fi | No MQTT/WS local listener or corresponding mDNS advertisement; upstream client still connects |
| Discovery | A1/M1/TC1, duplicate/invalid TXT, unsupported codec/version, offline and reboot | Only explicit supported peers accepted; bounded inventory; no implicit pairing |
| Owner management | Unauthenticated/expired owner session; empty/correct/wrong device password; unpair | Unauthorized writes rejected; correct pairing survives intended reboot; revoked peer cannot control |
| Route integrity | Forged mDNS, changed IP, explicit owner confirmation | No secret sent to an unconfirmed route; documented recovery succeeds |
| UDP reliability | Lost request/reply/chunk/ACK; duplicate and conflicting request ID; wrong token/source/port | Bounded failure and recovery; no duplicate actuation or cross-peer response delivery |
| State and controls | Initial and periodic reads; outlet/fan writes; sensor updates; stale state | Results update WebUI, HomeKit and forwarding; receipt alone cannot confirm unchanged/stale state |
| Local priorities | Control concurrent with discovery, UDP replies, HTTP and upstream traffic | Physical buttons remain responsive; no blocking discovery or unbounded admission wait |
| Upstream MQTT | External broker reconnect, retained data, binary NUL, upstream echo | Correct payload and identity; no forwarding loop or false local confirmation |
| Bemfa | Inbound text control; state forwarding; disabled/disconnected service | Correct conversion and failure isolation; no unexpected actuation |
| HomeKit | Real pairing, reads/writes, offline transition and recovery | Physical client evidence, separate from host simulation |
| HA | External broker discovery/state/control; broker unavailable | HA works without local broker; loss does not stop local UDP controls |
| Cloud versions | Simulated 0.9.x JSON and new MP devices concurrently | Discovery chooses codec per peer; no blind write retry with another codec |
| Resource bounds | Full build and actual stack-usage checks; large status and diagnostic replies | Image fits; unchanged stack reserves; bounded buffers, heap and queue behavior |

## Four-device comparison sequence

1. Freeze source commit, frontend commit/archive hash, exact model/variant, and
   compiler/linker/runtime versions. Build isolated GCC5/GCC14 MXCHIP candidates.
2. Snapshot mutable device configuration and health before each installation;
   verify model, version, uptime reset, Wi-Fi, MQTT and product health afterward.
3. Compare the same workload and duration against the original baseline, GCC5,
   and GCC14. Record request totals, complete/failed/busy/superseded counts,
   HTTP and operation P95, MQTT P95, minimum heap, and busy recovery intervals.
4. Include three simultaneous pages and three settings-tab changes within one
   second. Keep diagnostic sampling outside measured traffic except explicitly
   counted observations. Do not add recovery probes that amplify the load.
5. Complete real linkage/hub functionality before pressure and stability runs.
   Report actual elapsed stability time; a healthy short run is not a six-hour
   result. Busy recovery is unexercised when no busy episode occurs, not zero.

Device addresses and authorized recovery outlets belong to the private campaign
manifest. Revalidate the device and outlet mapping before any power operation.
