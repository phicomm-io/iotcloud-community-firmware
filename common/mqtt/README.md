# Shared MQTT owner

`phicomm_mqtt.a` compiles `core.c`, `client.c`, `session.c`, `service.c` and
`diagnostics.c` once per firmware. The three product adapters retain device
topics, legacy commands, HA entities and OTA callbacks. They do not own a second
connection scheduler or wire queue.

The protocol implementation remains the pinned MiCO/Paho source at SDK commit
`6c465211d3ff8797cd835e400ec54a06530dd476`, with the repository's existing SDK
patches. In particular A1/M1 retain their patched Yield loop, while TC1 retains
its single-cycle Yield. This module does not substitute another protocol library.

## Ownership and scheduling

* `ph_mqtt_owner_init()` binds the real RTOS thread. Only that thread may call
  Client/Network operations. Startup uses a generation-tagged atomic CAS; a
  previous creator cannot release a later worker. Published mutex/hooks remain
  stable through TC1 owner teardown and reinitialization.
* `ph_mqtt_session_run()` owns CONNECT, subscriptions, bootstrap, availability,
  ACL refresh, errors, retry backoff and cleanup. Adapter strings and subscription
  filters remain alive until Paho has been deinitialized. ClientInit and all
  partial connection failures close the Network and deinitialize Paho.
* Every Paho callback only validates borrowed bytes and copies one ingress item.
  The ingress guard admits each wire message once even with overlapping handlers.
  Modern, HA birth, feature, OTA and legacy business handlers run after the Paho
  operation has returned, including callbacks received during SUBACK/PUBACK.
* One poll executes at most one command and one external SEND. A telemetry turn
  allows at most four protocol publications. Product cursors retain unfinished
  discovery/state work; dirty updates do not erase first-pass coverage. TC1
  power/status attempts alternate. A1 urgent filter state alternates with pending
  ordinary leaves. These are operation/turn bounds, not wall-clock guarantees.
* Initial and subsequent HA rediscovery finish before periodic ACL refresh can
  replay retained birth. Fresh stop/identity checks run before online and after
  its PUBACK, before READY. A rename command's result is sent to its captured old
  target before the owner observes the configuration change and tears down.

## Memory admission

Queues are demand allocated, not fixed payload arrays. Limits are four COMMAND
items and six SEND items, including a popped in-flight item. Topic maximum is
511 bytes; payload maximum is 8192 bytes. TC1 preserves its smaller ingress
payload maximum of 2047 bytes; OTA retains its separate 640-byte validation.

The combined queue ledger is 12288 bytes. It includes each allocation's item,
topic, payload, terminators, a conservative header/alignment charge, in-flight
items and receipt credit. On the 32-bit targets an item header is 28 bytes;
charge is `(allocation_size + 31) & ~15`. Count/charge/free-heap checks precede
allocation and copy under the queue mutex. Replacement includes simultaneous old
and new charges. Clear closes admission until detached allocations are freed.

SEND is capped at 4096 of those 12288 bytes so telemetry cannot take the 8192
receipt credit. Queued retained SENDs may coalesce; commands, in-flight SENDs and
ordered non-retained messages do not. Expected generations bind external sends
to their original session; monotonic generations survive owner rebind.

A command starts with a bounded 160-byte failure credit. After parsing, it
consumes the original RX wire before upgrading to 8192 credit and executing a
mutation. This lets a valid 8192-byte command execute when resources permit.
Failure before that upgrade sends busy without execution. Two fixed emergency
busy slots preserve the original target/request ID when ingress is full. A
non-retained request that cannot enter either the queue or an applicable busy
slot is counted and dropped without execution or closing the connection.
`mqtt_ingress_dropped` counts these drops; `mqtt_busy_overflow` counts the subset
of modern commands whose two busy slots were full. Both counters saturate at
UINT32_MAX and follow the existing owner-reset lifetime. Retained admission
failures need no receipt and do not increment these drop counters. Broker PUBACK
acknowledges transport, not application admission or execution. Clients must wait
for an application receipt and may retry after a timeout; a missing receipt is
not proof of execution. Requests without IDs have no deduplication guarantee. The existing eight-entry SHA-256 dedup
cache preserves mutation fingerprints even if response encoding/caching fails.

This ledger is **not** total firmware memory or a partition of kernel RX heap.
It excludes parsed JSON, owned output wire, the existing eight receipt-cache
response strings, Paho RX/TX buffers, RTOS objects/stacks, TLS and closed socket
allocations. The 16 KiB application reserve remains a soft admission floor.
Network and other native allocations can overlap it. Modern service graphs use
the shared nonblocking JSON build lease; graphs/claims are released before any
synchronous sender/PUBACK wait. HTTP uses the same typed capture/pure encoder.

## Publish preflight deferral

`PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED` (3) means the unchanged native
packet/double-copy heap admission refused a publication **before** protocol
entry. It is distinct from turn/producer `PH_MQTT_MORE` and from an SDK error.
It records no socket failure and does not count a reconnect. Any failure after
`MQTTPublish` starts still follows the existing fatal session recovery.

The owner retains one command result's existing printbuf and captured target,
with its 8192-byte receipt credit, until send completion or teardown. It neither
copies that payload nor executes the command again. The fixed busy fallback is
copied into bounded owner storage. Pending result completion returns directly
to the session's stop/identity check, so a rename ACK reaches its old namespace
before another old-session command can execute. A real external stop takes
priority even if that ACK remains unsent. Teardown releases the wire/credit;
the existing dedup fingerprints survive session recovery, so an ID retry can
replay the cached response without executing the mutation again. ID-less
commands retain their existing lack of deduplication guarantees.

An external SEND stays in the core's in-flight slot and remains fully charged
against its item count, 4096-byte SEND cap and 12288-byte combined ledger. A
pending result keeps its existing credit charged; it does not add another
queue or another 8192-byte allowance. The result's existing native printbuf
remains outside that ledger; deferral retains the same payload rather than
creating a second copy. An accepted deferred result is at most 8192
wire bytes, plus its terminator, encoder spare capacity (at most 255 bytes),
printbuf metadata and native allocator overhead. Queued work plus credit stays
within 12288 bytes; this is still not a bound on total live native heap. A busy
emergency slot retires only on actual SDK entry; callbacks during PUBACK can
refill that slot without the returning send erasing the replacement.

Failed preflight attempts are separated by at least 1000 ms, using unsigned
time differences across wrap. The session gates native production before JSON
capture/encoding during that delay, and keeps bounded MQTTYield slices and
ordinary admitted ingress available. Receipts retain priority. Bootstrap
announcement waits with bounded Yield slices before subscribing; initial work
retains its adapter cursors; an unsent online availability never produces READY.
Previous-namespace cleanup remains the existing finite one-shot transaction:
a refused cleanup publication reports failure rather than claiming it was sent.

Each resource episode has a **30-second finite degradation limit**, measured
from its first refusal. Smaller successful publications do not extend it. The
owner keeps the exact first blocked native topic (no collision-prone digest),
and requires that topic to succeed plus a complete round without a pending
receipt/SEND/emergency or another refusal before ending the episode. Native
cursors advance only after success; for the same retained topic, a newer valid
sample may replace the old unsent sample. Owned receipts/SENDs are tracked by
their actual retained object, so a small same-topic native packet cannot retire
them. At expiry cause 7 (`PH_MQTT_CAUSE_RESOURCE_EXPIRED`) records the original
packet estimate with zero I/O evidence; the owner releases pending resources
and uses existing session recovery. This limit does not promise an application
receipt within a client's deadline, and prolonged pressure can still lose the
unsent receipt. It never silently turns expiry into send success.

Paho starts with separate 512-byte RX and TX buffers. Its storage overlay uses
explicit malloc/copy/free because the pinned platform realloc retains shrinking
blocks and copies beyond the old block when growing. Every replacement admits
the full new allocation against the existing live-heap reserve while the old
block is still live; old/new overlap remains additional native memory. Failure
keeps the old pointer and actual capacity, and a later operation retries shrinking.
RX growth copies only its encoded header; RX shrink preserves the prefix that
waitfor callers still parse. A streaming framing
guard checks canonical Remaining Length before Paho can allocate the advertised
packet; its packet ceiling is the model payload maximum plus 511+9 protocol/topic
allowance. Exact topic/payload bounds are checked at ingress. Malformed/oversize
framing poisons the connection through teardown instead of skipping bytes.

## Diagnostics

`ph_mqtt_snapshot()` copies a POD under a short mutex. It never exports topic,
password or body bytes. `/api/v1/memory` reports queued/in-flight counts,
queue/peak/credit bytes, rejects, owner faults, generation, delay, protocol depth,
operation, last error/stage and error/reconnect counters. Native RX/TX capacities
are sampled by the owner at protocol entry/exit; they cannot reveal an allocator
peak inside a blocked SDK call. No safe SDK runtime stack-watermark API is used:
runtime watermark is **NOT_AVAILABLE**, not zero.

The `/api/v1/memory` MQTT diagnostic fields `mqtt_last_error_stage` /
`mqtt_last_error` are the first recorded failure of
this session, or the most recent session with a known failure when the current
session has succeeded so far. The owner clears only a one-bit latch immediately
before each Network initialization (including old-namespace cleanup); it does
not erase the previous cause. A new session's first failure replaces it, even
when the cumulative error counter is saturated. Subsequent generic SESSION,
offline-publish or disconnect errors still count but cannot overwrite that
cause. An unclassified session failure stays SESSION with its original return
code: this is an observation, not proof of the underlying socket/reset cause.
A normal stop with return zero creates no error event. RECONFIGURE retains its
existing cumulative event count, but does not replace the failure cause; a real
error during its cleanup can still become that session's first cause. Thus
`mqtt_errors` counts reported events (including lifecycle/repeated cleanup
notifications), not distinct failed sessions. Successful reconnects retain the
previous cause; a subsequent owner rebind still resets the diagnostic core.


HTTP diagnostics allocate a request-owned 2304-byte scratch only after checking
the application reserve. It lives until response cleanup. On insufficient heap
or allocation failure the original smaller response includes
`mqtt_diag_unavailable:true`; it does not fabricate zero counters or enlarge
permanent BSS/thread stacks. Counter-expansion fixtures cover UINT32_MAX and
INT32_MIN, valid JSON, low-heap fallback and exact cleanup.

## Cleanup exception and closed-kernel boundary

Previous-namespace cleanup preserves the existing synchronous transaction.
It is an exception to the four-publication ordinary turn, not a globally bounded
four-operation scheduler. At `FEATURE_MAX_PEERS=4`, A1 removes at most 49 entities:
four physical, six filter and 39 virtual (`11 + 4*7`). M1 removes six and TC1
17. Reconnecting for old-namespace cleanup can additionally CONNECT, announce,
publish offline and DISCONNECT: at most 53/10/21 protocol operations respectively
(legacy announcement may be absent). A connected closing path omits reopening.
Errors stop virtual cursor progress; fixed physical cleanup loops remain finite.

Every such operation has a configured 5000 ms command timer, yielding nominal
timer sums of 265/50/105 seconds for that reconnect transaction. These sums are
**not a proven wall-clock timeout**: pinned Paho packet/response handling can
start additional timers, and closed DNS/socket/close calls are not proven finite.
The application performs finite CPU work and normally blocks/yields in socket
waits; it cannot prove closed-kernel scheduling or prevent a kernel deadlock.
Cleanup does not hold state/JSON/queue locks across network I/O. No watchdog is
disabled or fed to conceal a stalled operation. Kernel close/resource reclamation remains an external release gate.

## Build and stack gates

`tools/check_mqtt_core.py` reads actual model and shared-archive `.su` frames.
Capture, encoding, deferred control, protocol ingress copies, pending receipts,
bootstrap, teardown and diagnostic/adapter tails are checked as separate reachable
paths, with owner/session frames that remain live. Mutually exclusive paths must
not be added as nested PUBACK business callbacks. ELF gates require one shared
session, poll and enqueue implementation. Keep configured stacks (A1/M1 16384,
TC1 8192 bytes) and conservative SDK/native/Paho/RTOS reserves (4096/4096/3072)
explicit. Static frame sums and allocation ledgers are not runtime watermarks or
proof of closed-kernel stack/heap safety.

## Regression entry points

From the repository root:

```sh
python3 -m unittest tests.test_shared_mqtt tests.test_mqtt_preflight_deferral tests.test_mqtt_stack_budget tests.test_mqtt_buffer_storage
```


`tests.test_shared_mqtt` uses actual shared production code and isolated copies
of the pinned, product-patched Paho sources. It covers fragmented packets,
SUBACK/PUBACK ingress-only callbacks, queue/credit/cleanup, 8192-byte real service
commands, dedup/conflict/original rename target, per-allocation failures,
partial ClientInit, retries, >40-second bootstrap/rediscovery and startup/rebind
concurrency. Product scheduler/lifecycle tests cover continuous dirty/filter/
power work and bounded progress. ARM/QEMU JSON/native-budget tests and actual
three-model builds complement ASan/UBSan. The existing isolated-broker suite
continues to exercise real product workers, HA/legacy/filter/fan/CO2/resource
semantics; none of these host proofs replace physical load/recovery/soak tests.

### Bounded failure classification

`mqtt_failure` accompanies the existing first-session-error latch. `operation`
is the failing operation captured at error time, whereas `mqtt_operation` is the
current live operation. The object persists through teardown and healthy retries
until a later session records its first failure. `reason` values are 0 unknown
(session/network outcome), 1 SDK return with no classified adapter failure,
2 publish heap admission denied before allocation, 3 shared RX framing rejection,
4 legacy ingress receipt capacity exhausted (not generated for overload drops), 5 negative/short adapter read, and 6
non-positive adapter write. These categories describe observed paths, not root
causes. In particular SDK does not imply a bug in the SDK.

Application source stages extend the same three stage fields through 14:
8 discovery graph construction, 9 discovery identity construction,
10 discovery encoding, 11 OTA graph construction, 12 OTA encoding,
13 TC1 namespace/send conversion, and 14 command receipt-credit admission.
They retain reason 0: the label identifies the observed failing boundary, not
an allocator or network root cause. Only an immediately fatal negative return
marks a source; it does not increment error counts. The existing SESSION
summary commits that source once. A real protocol failure already latched wins.
Protocol begin/end and the next session begin clear the transient operation;
previous immutable failure evidence remains available through healthy retries.
There is no additional persistent state, queue, allocation, or configured
thread-stack-size/reserve change. Periodic telemetry's existing MORE policy is
unchanged; uninstrumented application failures can still remain SESSION/0.

`free` is live free heap sampled when the error is recorded; `packet` is the
publish packet-size admission estimate (zero for other operations), and `credit`
/ `queue` are the locked shared ledger values. `rx_state` is the framing guard
state (0 header, 1 remaining length, 2 body); it is meaningful only after
protocol initialization, and may belong to the previous parser for a network-open
failure. `read` and `write` are fixed arrays
`[requested_bytes, returned_bytes, supplied_timeout_ms, elapsed_ms]`. All-zero
arrays mean no adapter call was recorded for that operation. A zero read means the adapter returned no bytes; depending on the opaque
adapter this can be idle/deadline evidence and alone does not classify a transport failure.
Positive partial writes remain supported. The first classified anomaly freezes
both adapter observations, preventing subsequent protocol ACK/cleanup calls from
replacing its evidence. Successful operations clear temporary observations.

No payload/topic/credentials, errno assumptions, additional socket operations,
dynamic diagnostic allocations, configured thread-stack size changes, or retry
changes are involved.
Opaque adapter negative returns cannot distinguish select failure, recv failure,
or peer close. Deadline evidence cannot establish a peer or kernel root cause.
The MQTT frame checker reports diagnostic error-record and guarded-adapter tails
separately for each retained session/product publisher. Error recording includes
an emitted `end` frame only after Paho has returned; it does not include sibling
RX ingress or JSON encoding frames. Read owner checks, read adapters and write
callbacks remain separate live paths beneath the unchanged SDK reserve. Required
emitted diagnostic/owner-hook symbols must be present. TC1 namespace conversion
returns before publication, so its `account_message` frame is excluded from these
new publisher tails. Host budget regressions reject missing frames and injected
oversized callbacks/error recorders, and verify mutually exclusive tails are not
summed. This extends the earlier aggregate checks to cover the new instrumentation.

### Recoverable ingress overload

Queue admission failure is application backpressure, not a socket error. The
callback preserves the two bounded modern-command busy slots before counting a
non-retained drop; it performs no business execution, JSON allocation, or send.
Nonmodern input has no applicable emergency receipt and is counted directly.
These counters distinguish actual drops from `mqtt_rejected`, which also counts
admission failures covered by an emergency busy receipt. Reason 4 is retained
as a legacy diagnostic value; this overload path no longer emits it.

The actual Paho host fixtures cover nonmodern full queues, four modern entries
plus two busy slots and a seventh dropped command, retained refusal, oversize
input, heap/allocator refusal, and refill during a busy PUBACK wait. Real service
fixtures prove the dropped command did not execute, a later identified retry
executes once with SHA replay protection, and rejected ID-less controls never
execute. Genuine SDK transport failures remain fatal. The existing rename,
result-credit, per-allocation, and product scheduler checks remain applicable.
