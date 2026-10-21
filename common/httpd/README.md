# Shared HTTP core

`parser.c` and `core.c` are application-owned GPL-3.0-or-later code shared by
A1, M1 and TC1. The MiCO port uses the public socket/RTOS ABI of SDK 3.6.4,
commit `6c465211d3ff8797cd835e400ec54a06530dd476`. No vendor HTTP parser,
registry, SSI, FTFS or daemon is linked. Derived product adapters retain their
original MIT notices.

## API and ownership

`http.h` defines the byte parser, `ph_http_io`, `ph_http_app` and server API.
`ph_http_server_open/step/close` run the core; `ph_http_start/stop` provide the
MiCO owner lifecycle. Stop is cooperative: socket/core storage stays owned until
the live owner acknowledges. `ph_http_snapshot()` copies cached diagnostics;
`ph_http_json_owner()` exposes the stable handle only on the HTTP thread.

One 12288-byte owner stack owns all sockets from accept through close. Static
slots are A1=3, M1=4 and TC1=4; the configured TCP cap matches these limits and
becomes 2 during OTA. Headers parse independently, but only one exchange calls
business code or owns a body/JSON response. One completed header may wait one
second; further completed headers receive bounded 503 replies. Partial headers
do not hold the business lease. Application callbacks are synchronous and cannot
be preempted by the event loop.

The application `begin/retry/body/complete/release` callbacks implement admission,
body handling and response ownership. The response body, owned JSON printbuf and
reservation survive through send/close. `release(response_sent)` receives true
only after complete transmission and successful descriptor close; it does not
prove that the peer application received the response. Static assets stay in
flash. The scalar response buffer is 1280 bytes on A1/M1 and 1536 on TC1.

## Bounds and failure behavior

Headers have a five-second absolute deadline, 4096-byte total budget, 64 nonempty
header lines and a 512-byte line buffer (511 payload bytes plus NUL). A completed
slot reuses its parser line buffer for at most 127 carried body bytes. Responses
have a ten-second absolute deadline. Malformed/overload replies and unread-body
draining are bounded by 100 ms; draining is also capped at 8192 bytes. Connections
are short. Transient I/O yields 1 ms. OTA uses the shared 512-byte scratch.

Listener and accepted descriptors must be nonblocking and in [0,63]. Listener
admission is configured after listen and checked by getsockopt; unsupported
fcntl/readback fails closed. Optional accepted-socket keepalive/TCP_NODELAY calls
are omitted because pinned kernels implement socket options via synchronous
TCP/IP callback waits. The application memory reserve is a soft admission
threshold, not an SDK RX heap partition or a minimum free-heap guarantee.

The port cannot classify negative socket results using application newlib
`errno`: the pinned kernels write another error store and expose no public
socket errno getter. Diagnostic error 0 for a negative result means unknown.
Recv/send negatives retry within the existing absolute/idle deadlines; EOF is
immediate. Select allows two yielding retries, then the third consecutive failure
enters owner restart; a completed wait resets the streak. Accept negatives yield
for a 250 ms elapsed window while existing clients are serviced, then enter the
restart budget; successful accept or a completed wait without listener readiness
resets that window.

Other fatal startup/I/O failures get at most three clean restart attempts.
Exhaustion leaves HTTP unavailable. Failed close is recorded, never retried on a
potentially reused global fd, and stops service with automatic restart blocked.
This contains application behavior; it proves neither recovery nor absence of a
leak. No watchdog is disabled or fed to conceal these failures. Kernel calls,
Flash callbacks and config-lock acquisition can themselves block: application
deadlines do not establish finite closed-kernel execution or resource reclamation.

## Protocol compatibility

| Behavior | Adapter contract |
| --- | --- |
| GET service APIs | Existing `/api/v1` info/status/capabilities/config/logs/schedules/memory dispatch and full/live views |
| POST command | 8192-byte body bound; TC1 rejects empty body; OTA permits at most 1024-byte commands |
| OTA upload | Existing signed/explicit legacy-raw mode and writers; verify cap 2 before synchronous setup; 512-byte chunks, 15-second idle/300-second total deadlines |
| A1/M1 frontend | `/`, `/index.html`, `/favicon.ico`; gzip, ETag/weak match, private cache and Vary Authorization |
| TC1 frontend | `/` and `/assets/index.html`; `/assets` prefix reaches adapter, other assets return 404; gzip |
| TC1 aliases | Query immediately after registered URI and trailing slashes match; arbitrary suffixes do not |
| HEAD | A1/M1 authenticated 404; TC1 GET route matching and GET Content-Length, headers only |
| OPTIONS | A1/M1 before auth/route; TC1 after route match and before auth; existing trusted origins |
| Basic/CORS | Product realms/origins retained; initial TC1 auth failure closes, atomic replacement preserves prior credentials |
| Other routes | TC1 `/api/v1/http-trace`; A1 light-control callbacks and OTA exclusion retained; unregistered legacy handlers stay inaccessible |

TC1 HTTP OTA verifies first and returns 202 in `verified` state. Only a fully
sent response and successful close activates the boot slot, followed by the
existing 300 ms grace/reset. Failed send/close discards staging logically,
reports failure and releases application reservations without automatic reset;
a close error still stops the HTTP owner. Activation uncertainty retains both
reservations and prohibits automatic retry/reset. MQTT/download retains its
verify-and-activate-before-success flow. A1/M1 activation uncertainty likewise
retains reservations: a RAM-table restore cannot undo partially persisted boot
metadata. No public safe rollback is assumed.

A1/M1 failed OTA restoration retains the job in `restoring`; explicit HTTP-owner
and MQTT-loop polls attempt resumption at most once per second. The getter is
pure. An in-progress token prevents concurrent starts; the claim clears only
when the binding worker is ready. Permanent allocation failure can remain
indefinitely degraded. Boot readiness waits at most five seconds. Only top-level
OTA operations release ordinary failures; low-level Flash errors and repeated
cleanup cannot release a subsequent job. A pause request leaves the worker
watchdog active until acknowledged exit, before close/cleanup.

## JSON and status contracts

`common/json_owned.c` consumes one owned response reference using the patched SDK
private JSON layout. It retires encoded members only through uniquely owned
ancestors; shared roots/subtrees remain unchanged. SDK primitive callbacks retain
escaping/number bytes. The reusable primitive buffer and 256-byte wire growth
avoid a second full graph lifetime. Traversal uses 32 automatic frames; excessive
depth, sticky builder errors or OOM discard the complete wire. Existing cycles
remain the caller's responsibility to break. This does not change SDK-wide JSON.

HTTP and modern MQTT share a non-waiting generation token for JSON construction
through encoding. A short mutex protects only scalar ownership, never network or
builder work. HTTP small-request contention uses the two-second WAIT/25 ms retry
policy; cleanup releases token and byte claims exactly once. Encoding releases
the token before transmission. The unchanged 16 KiB soft floor and selected
producer exclusion do not isolate memory from discovery/config, automation or
native SDK/network allocations. Background owned publication consumes its graph
before synchronous QoS1 transport and retains independent wire through transport.

Status uses request-local typed projections from `common/device_status.h`.
HTTP/MQTT share model capture and pure encoding. A1/M1 capture uses config→state
mutex order and each feature/watchdog owner; TC1 uses its status mutex and
config-phase exclusion, with OTA metadata copied separately. Initialization
failure stops TC1 startup. Groups are individually coherent, not one atomic
hardware instant; no state mutex spans GPIO, JSON, allocation, network or Flash.
Config reservation rejects captures throughout restore/persistence/rollback;
OTA reservation alone permits coherent live snapshots.

The snapshot clock is sampled after sensor copies. Encoding reads frozen POD
and immutable literals only, with no getter, clock, lock or hardware access.
Live omits full diagnostic/scan/credential scratch; full includes diagnostics
and password-presence booleans. M1 retains CO2-only SCD40 exposure; A1 reports
last effective fan output without running PID. Heap metrics are captured before
JSON allocation. A1/M1 full status omits the seven added HTTP owner/restart/close/
size fields; their complete cached values remain at `/api/v1/memory`.

## Diagnostics and verification

`http_core_closed` order is success, invalid header, deadline, peer EOF, I/O fault,
busy, stop. Counters saturate; socket metadata contains descriptor/return/error
numbers only. `http_core_occupied` is current userspace occupancy.
`http_tcp_sample_ms` timestamps the last owner-only listener admission snapshot
(startup or actual cap change); TCP_CONN_NUM is not live. Steady owner waits and
status/MQTT readers perform no observation getsockopt calls. MQTT diagnostics
use a request-owned 2304-byte scratch after reserve admission; OOM/low heap keeps
the smaller response with `mqtt_diag_unavailable:true`, never fabricated zeros.

Run host regression modules from the repository root, for example:

```sh
python3 -m unittest tests.test_http_core tests.test_http_callbacks tests.test_shared_http_adversarial tests.test_http_json_owned tests.test_json_build_gate tests.test_status_views tests.test_typed_tc1_status tests.test_ota_recovery
```

Additional entry points include `tests/test_http_core_tcp.py`,
`tests/test_http_json_budget.py`, `tests/test_shared_http_arm.py`,
`tests/test_shared_http_soak.py`, `tests/test_shared_http_tcp_soak.py`,
`tests/test_tc1_status_guard.py`, and `tests/ota/run.sh`. Firmware gates use
`tools/check_http_core.py` and model/common `.su` frames. Verify the available
cross-toolchain/QEMU and SDK setup for ARM fixtures. Host parity, fault injection,
allocation ledgers and static stack sums do not establish physical latency,
stack watermarks, ordinary-load rejection rate or long-term heap stability.
Physical cap/nonblocking support, close/recovery and concurrent HTTP/MQTT load
remain separate device checks.
