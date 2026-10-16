# A1/M1 stability review and validation

Status: in progress. This report is not an acceptance or merge record yet.

The implementation branch is `fix/a1-m1-stability-validation`, based on
`3eddfd56bc312089e726bb2d362924e2db53e369` (`origin/main`). An isolated worktree preserves the
original checkout's A1 calibration work. No factory reset or power-cut OTA test
is authorized or performed. No release tag is created.

## Changes and review

- Retain the request ID and original request when allocating either the result
  object or cached response fails after a mutation. Retrying that request returns
  busy without executing it again. The eight-entry cache frees both owned strings
  on eviction; the regression covers 100 evictions and allocation failures before
  mutation, after mutation, and while constructing the result.
- Rule-only saves no longer reconnect MQTT when the subscription set is unchanged.
  The filter comparison frees its temporary allocation on all normal paths.
- The binding worker stack is 12 KiB and M1 MQTT stack is 16 KiB. MQTT-owner-only
  snapshots avoid large nested callback stack frames. Compiler stack-usage guards
  include explicit SDK reserves; these are conservative application call paths,
  not measured runtime watermarks or a formal proof of all SDK paths.
- HTTP listener/socket capacity matches the prior diagnosed MiCO connection limit:
  backlog 8 and SDK maximum 12. Single-thread request processing still has bounded
  header/body deadlines; an incomplete client can temporarily delay other clients.
- Status adds uptime and SDK allocator snapshots. `min_observed_free_memory` is a
  sampled minimum, not the allocator's exact low-water mark. None is persisted.
- Preserve verified A1 color order, calibrated ring fill, touch/key feedback, panel
  I2C recovery, and repeated light-effect saves. Remove temporary LED test route.

Ownership review includes HTTP body/parser lifetimes, MQTT connect/disconnect and
reply queues, OTA URL worker ownership, runtime queue/config/sample/RPC lifetimes,
group pending replacement/timeout, and automation transaction copies. Bounded
retained allocations are not described as leaks. Sanitizers and on-device idle
heap comparisons supply separate evidence; no claim of eliminating every possible
allocation failure in the pinned JSON library is made.

- JSON floating conversion uses a call-private newlib reentrancy context, reclaimed
  after both formatting and parsing. The linked mocOS image has no RTOS reference
  to `_impure_ptr`; the old JSON library used the shared context across HTTP/MQTT
  and binding threads. This removes a source-level concurrency risk. The first
  candidate had one malformed binding-status response at Python character 235,
  near the temperature value in a later successful response. Its original body
  was not saved, so this is not a proven causal diagnosis. That four-client test
  failed; a separate five-minute reproduction (4568 requests, zero errors) does
  not erase it. All final-candidate JSON responses are parsed and malformed bodies
  retained privately for diagnosis.

- Group retries own a deep copy instead of sharing non-atomic json-c references
  with the dispatch thread and its nested request body. Probes transfer ownership
  and recheck that the slot is vacant under lock, avoiding a concurrent-control
  overwrite/leak. A regression mutates the original control body after fan-out:
  it fails before the fix and verifies the retry remains the original command
  afterward. An earlier host leader exited with SIGSEGV during recovery; no core
  was available, so causality is not claimed. A later interrupted host query also
  exposed reply desynchronization in the test harness; correlation IDs and raw
  pipe reads fix that harness issue separately.

- JSON serialization failure is checked before cache copies, HTTP replies, runtime
  queues, MQTT payloads, light parameter copies and OTA state formatting. Failed
  result serialization after a mutation retains the receipt and never executes
  the mutation again; allocation-failure regressions cover both sides.
- A subsequent ASan fleet run captured a null topic-data dereference in SDK MQTT
  dispatch while a coordinator rejoined. The initial interpretation that the
  filter pointer was null was incorrect: source line 248 dereferences the topic
  data. `MQTTDeserialize_publish` can return the positive remaining-length decoder
  result after topic parsing fails, incorrectly accepting a nonzero topic length
  with null data. The bounded parser patch and malformed-packet regressions pass
  in the host suite; the group ownership fix is separate. The specific network event that
  produced the malformed parse in that fleet run is not yet established.

## Evidence boundaries

Host sanitizer regressions exercise production parsers, service, engine and runtime
transactions with test adapters. MQTT protocol tests execute the native workers
and SDK MQTT client against Mosquitto, but stub feature hooks and therefore do not
prove full binding behavior.

`tests/simulator/` additionally executes actual production service, automation,
binding workers, grouping, engine and MQTT workers in separate POSIX processes.
Only Flash, RTOS and physical peripherals are replaced. Each process binds a
separate loopback IP for real UDP, and all use the dedicated test Broker. The UDP
relay modifies real packets for reversible faults. This is software/network
proof, not hardware timing or MCU stack/heap proof.

The A1 and M1 physical devices are independently tested with signed image hashes,
identity, configuration retention and runtime health. The user confirmed normal
M1 screen display and A1 keys/light strip on the initial candidate. API-only
brightness/RGB requests do not prove physical optical output.

### R10 device and fault evidence

R10 was built from source version `0.9.5-3eddfd-dirty` and deployed to A1
`b0f893303134` and M1 `b0f8932864b6`. Both returned HTTP 202 for upload, then
reported the expected version, retained configuration, and healthy Wi-Fi and
hardware. Device upload acceptance is not by itself proof of boot; subsequent
device status and read-only diagnostics provide the runtime evidence. The SDK
`TCP_MAX_CONN_NUM` set/readback was 12 on both devices.

- A1, four concurrent clients for 180.259 seconds: 2,942 requests, zero errors
  or resets, P95 0.505 seconds. Thirty-two health samples showed continuous
  uptime and panel, fan, Wi-Fi, and MQTT readiness. Evidence:
  `build/evidence/a1-qa/r10-diagnostic-load4/`.
- M1, four concurrent clients for 180.050 seconds: 720 requests, zero errors or
  resets, P95 0.430 seconds. Thirty-five health samples showed continuous
  uptime, MQTT readiness, valid sensor data, and recovery in 0.103 seconds.
  Evidence: `build/evidence/m1-qa/r10-diagnostic-load4/`.
- M1, eight clients started together for 30.011 seconds: 240 requests and 31
  `ECONNRESET` errors spread across routes, with no controlled 429/503 response.
  Uptime remained continuous, MQTT and sensor status stayed healthy, and API
  recovery was 0.130 seconds. Sampled connections peaked at 9, below the
  configured limit of 12. Evidence:
  `build/evidence/m1-qa/r10-diagnostic-load8/`.
- M1, eight clients started 200 ms apart for 30 seconds: 237 requests, zero
  errors, P95 0.307 seconds; uptime and MQTT/sensor status stayed healthy.
  This single contrast run does not prove that staggering fixes the cause.
  Evidence: `build/evidence/m1-qa/r10-diagnostic-load8-staggered/`.

The reset source remains unresolved. Source review found a serial HTTP accept and
request worker, but no intentional RST path. The initial runs had no packet
capture; the later capture and admission comparison below narrow the failure.
The successful staggered run does not explain the
31 resets in the simultaneous-start run. This is a release blocker, and no
production socket-policy change is justified by the current evidence.

Two further simultaneous-start repetitions, each eight clients for 20 seconds,
also failed. They recorded 354/550 and 360/562 failed requests respectively;
the first observed reset was on `/api/v1/logs` at 166 ms and `/api/v1/config` at
163 ms. Three of four health probes failed in each run, so device-side health
observation was degraded during the burst. These uncapped diagnostic repetitions
confirm reproducibility under this harness but are not the planned paced 2-minute
acceptance load. Evidence:
`build/evidence/m1-qa/r10-repeat-load8-a-requests.ndjson`,
`build/evidence/m1-qa/r10-repeat-load8-a-health.ndjson`,
`build/evidence/m1-qa/r10-repeat-load8-b-requests.ndjson`, and
`build/evidence/m1-qa/r10-repeat-load8-b-health.ndjson`.

A third 20-second simultaneous-start repetition also failed: 400 of 594 requests
failed, beginning with `ECONNRESET` on `binding.status.get` at 166 ms. Two of four
health probes failed. Evidence: `build/evidence/m1-qa/r10-repeat-load8-c-requests.ndjson`
and `build/evidence/m1-qa/r10-repeat-load8-c-health.ndjson`.

### Restored-device paced retest and packet capture

After device recovery, the same R10 M1 identity and version were verified before
running eight simultaneous clients at one request per client per second for
120.036 seconds. Of 960 requests, 144 failed with `ECONNRESET`; none was a
controlled HTTP rejection. P95 was 0.658 seconds. Six of 23 health probes reset;
the 17 successful probes had valid sensor data and MQTT ready/connected. The
MQTT error counter was already 2 at baseline and did not increase. Observed
uptime stayed continuous, and API/sensor recovery took 0.125 seconds. Evidence:
`build/evidence/m1-qa/r10-restored-8x120/`.

The corresponding 200 ms staggered-start comparison passed 237/237 requests over
30.811 seconds, P95 0.384 seconds. Six health samples were valid, with no reset,
and API/sensor recovery took 0.111 seconds. Evidence:
`build/evidence/m1-qa/r10-restored-8x30-staggered/`.

A scoped host-side capture (M1 TCP port 80, 128-byte snapshots) collected 22,200
packets with zero capture-kernel drops. It overlapped the paced and staggered
runs, but did not cover the complete first run. Its 131 reset flows all had
client request payload and no preceding server payload. These are flows, not
application error counts; retransmitted RST packets must not be counted as
separate failed requests. Evidence and the packet-summary driver:
`build/evidence/m1-qa/r10-paced-capture/`.

A separate one-shot admission comparison opened 4, 6, then 8 TCP connections,
held them for 200 ms before any HTTP request, and then sent valid status GETs.
The corrected driver showed all four and six connections succeeding. With eight,
one connection (client port 60802) received RST before sending any HTTP bytes;
the other seven GETs succeeded. The packet capture confirms its SYN, SYN/ACK,
ACK, then server-address RST, without client request payload. All three corrected
rounds finished within 0.9 seconds and recovered API/sensor/MQTT readiness.
This excludes HTTP JSON parsing and business routing as the direct trigger of
that reset, but does not establish the closed SDK's internal queue capacity.
Evidence: `connect-admission-corrected-*` in the staggered evidence directory,
and `build/evidence/m1-qa/r10-admission-capture/`.

The first admission driver incorrectly passed a buffered stream instead of a
socket to `HTTPResponse`. Its HTTP errors are harness errors, not device errors;
the original evidence is retained separately from the corrected runs.

R11 changes M1 to accept sockets in a separate 2 KiB thread into a fixed
eight-entry queue. The existing request worker remains serial, preserving
mutation and OTA processing behavior. Successful queue insertion transfers socket
ownership; queue overflow attempts a nonblocking 503 and closes the socket.
Queue/thread startup failures close the listener and release initialized queue
resources. The TCP client limit remains 12. This targets delayed application
accepts; overload delivery and hardware improvement require device tests and are
not assumed from the source change. The read-only status field
`http_accept_queue_capacity=8` distinguishes this implementation from R10, whose
dirty-version text is otherwise identical.

The R11 M1 ARM build passed with an accept-worker application frame of 16 bytes
plus 1536 bytes SDK reserve within the allocated 2048 bytes. This is a static
budget check, not a measured runtime watermark. Frozen source identities:

- `M1/http.c`: `e5fb7049341ecb3bfa2f2fdc57ced0f3676ae8af0b39731c5f764a88702a0028`.
- `tools/check_worker_stack.py`: `cfaaa33366484ce3a2172cd199da119e042848335a4e7f606b2a52ade561f28d`.
- M1 raw OTA: `18ca97896f17b484a280a4c760a3284b7f14a4e496f211377bed0dcdc6b73975`.
- M1 signed OTA: `50393d9a28404d737bef25ed5d0562f1e9ccce566b3a5666f3b99f645c66c2fa`.

The single signed upload returned HTTP 202. After reboot, M1 reported the new
capacity field as 8, uptime decreased from 23,807,700 ms to 36,760 ms, and Wi-Fi,
MQTT and sensor readiness passed. All eight checked configuration fields were
retained. Free memory changed from 50,628 to 48,516 bytes (an observation, not a
leak or stability conclusion). Evidence: `build/evidence/m1-r11-deploy/` and
`build/evidence/m1-r11-build/`. R11's corrected 4/6/8-client admission comparison passed all 18 requests,
with zero pre-request resets and API/sensor/MQTT recovery after every round.
The 8-client, 120-second, one-request-per-second-per-client retest then passed
960/960 requests with zero failures or controlled refusals (P95 0.768 seconds).
All 23 health probes passed, sensor validity and uptime continuity passed,
and recovery took 0.113 seconds. This contrasts with R10's 144/960 failures
under the same paced load. Sampled heap ranged from 37,856 to 48,532 bytes;
this short test does not establish a leak-free long run. Evidence:
`build/evidence/m1-qa/r11-validation-admission/` and
`build/evidence/m1-qa/r11-validation-paced/`. Higher load and long acceptance
remain pending.

R11 also passed the isolated 8-client, 20-second unpaced diagnostic:
233 requests, zero failures/refusals, P95 0.880 seconds, recovery 0.105 seconds.
The subsequent 16-client paced stage failed: 1,920 requests in 120.403 seconds,
369 uncontrolled failures (289 connection resets and 80 root-path responses
whose status/body size were not recorded) and 422 controlled refusals; recovery was 0.132
seconds and sampled uptime remained continuous. The health monitor recorded four connection resets and 13 controlled busy
responses. Only six health samples succeeded; successful samples showed valid sensors. Successful health samples
alone do not mean the health path was error-free. Evidence:
`build/evidence/m1-qa/r11-validation-unpaced/` and
`build/evidence/m1-qa/r11-validation-16x120/`. Packet captures are retained in
`r11-accept-capture`, `r11-accept-capture-continuation`, and `r11-16-capture` under
`build/evidence/m1-qa/`; their windows overlap and must not be summed as unique
requests or treated as application outcome counts.

A1's matching R11 queue implementation built successfully, but was not deployed.
R12 changes the fixed queue to 16 entries and requested TCP client limit to 24
in both models; the request worker and listen argument remain unchanged. This is
a capacity hypothesis for 16 load clients plus a health probe, not proof of the
opaque MOC network stack's capacity or per-connection memory cost. The requested-limit status field and socket option now share one constant to
avoid misleading diagnostics. R12 A1/M1 ARM builds and normal/ASan+UBSan host suites passed. Deployment
readback, peripheral recovery and load validation remain pending. The test harness now retains status/body size for
failed root-page responses, correctly recognizes JSON busy responses on that
path, and saves summaries before acceptance assertions; original R11 evidence
is not retroactively reclassified.

R12 M1 signed OTA returned 202 and rebooted successfully. Queue capacity 16,
requested/readback TCP limit 24 and set/get return codes zero were verified;
Wi-Fi/MQTT/sensor readiness and eight retained configuration checks passed.
The 4/8-socket admission rounds passed, but 16 sockets produced only 13 HTTP
successes: clients 13/14 observed EOF before requests and client 15 disconnected
without a response. Recovery took 1.229 seconds, uptime remained continuous,
and MQTT errors stayed zero. The 120-second load stages were not executed after
this admission failure. Capture confirms FIN without server payload for the
three connections, followed by RST after client requests; it does not identify
which device-side layer closed them. A1 R12 deployment was paused pending this
investigation. Evidence: `build/evidence/m1-r12-deploy/`,
`build/evidence/m1-qa/r12-validation-admission/`, and
`build/evidence/m1-qa/r12-capture/`. No R12 acceptance or merge is claimed.

M1 R13 adds read-only, accept-thread-owned saturating diagnostic counters for
accept success/failure and errno, last fd, queue success/rejection and return
code, nonblocking busy-send outcomes, and explicit overflow closes. Capacity,
request serialization and refusal behavior remain identical to R12. The frozen
`M1/http.c` SHA-256 is
`f1e25e471b44bc706a0da8c7d562d96150e1098d344f99df422b81a00dbb11f8`.
R13 signed OTA returned 202, rebooted, and passed identity, diagnostics,
Wi-Fi/MQTT/sensor and eight configuration-retention checks. The accept static
stack budget was 24 + 1536 = 1560/2048 bytes, and signature tests passed 7/7.
Evidence: `build/evidence/m1-r13-build/` and `build/evidence/m1-r13-deploy/`.
A repeat admission comparison will distinguish visible application rejection
from failures before application acceptance; this diagnostic is not a fix or an
acceptance result.

R13 reproduced the 16-socket failure. Adjusting for the after-status GET,
13 probe sockets were accepted/enqueued and three accept failures were recorded.
Queue rejection and explicit overflow-close counters stayed zero; last accept
errno was zero, so no useful errno diagnosis was available. This locates this
specific failure before successful application acceptance, not in queue overflow.
It does not prove a particular global descriptor limit in the opaque SDK.
Evidence: `build/evidence/m1-qa/r13-validation-admission/` and `r13-capture/`.

R14 tests one change: queue depth returns to 8 while TCP limit stays 24 and the
read-only diagnostics remain. It tests whether accepting and promptly refusing
excess clients avoids retaining enough descriptors to trigger the R13 failure;
no result is assumed. Frozen M1 source SHA-256:
`cbb3e94dcd31e8f392e90ec22799af003c76c4bdf05f4d81b49bfd0e1624bfb6`.
R14 ARM build, signature verification and 7/7 signature tests passed; the accept
static budget remained 1560/2048 bytes. A single signed OTA returned 202, then
queue 8 / TCP readback 24, readiness and eight retained configuration checks
passed. Candidate signed SHA-256:
`85d16a6aa0729a58fafbf5e8efc9a6bf74c0ac3bdb5280b469994677badc447c`.
Evidence: `build/evidence/m1-r14-build/` and `build/evidence/m1-r14-deploy/`.
R14 completed 120.894 seconds: 1910 requests, 1237 HTTP 200, 672 controlled
busy replies and one root-path connection reset. All 22 health requests were
successful (16 status, six controlled busy), uptime stayed continuous, MQTT
errors stayed zero, and recovery took 0.134 seconds. The gate still failed.
Accept failures were zero; all 678 overflow replies (workers plus health)
reported successful sends and explicit closes. The failing connection's packet
trace shows client GET followed by RST with no response payload. These facts
motivate testing whether unread request data causes close to discard a busy
reply; they do not prove that mechanism. Evidence:
`build/evidence/m1-qa/r14-validation-16x120/` and `r14-capture/`.

R15 keeps queue 8/TCP 24 and, only in the nonblocking busy path, drains at most
eight 128-byte chunks already available before sending the rejection. A positive
receive increments `http_accept_busy_drained`; this counts read operations, not
bytes. No rejected request is executed. Frozen M1 source SHA-256:
`10d36db697571550e4c1f401fc6f2a98d624daa9e83ff376b5bb8f6b73e56134`.
R15 built with accept stack budget 152 + 1536 = 1688/2048 bytes and passed
7/7 signature tests. Single signed deployment returned 202 and runtime/config
retention checks passed. The added drain counter was inadvertently omitted from
status serialization in this image; the deployed source still increments it,
but it is unobservable and must not be reported as measured. Existing accept,
queue, send and close counters remain available. The current image is being
retested for request outcomes; the next frozen candidate must also include the
missing serialization. Evidence: `build/evidence/m1-r15-build/` and
`build/evidence/m1-r15-deploy/runtime-smoke-diagnostic-gap.json`.

R15 passed the 16-client/120-second stage: 1913 requests in 120.959 seconds,
zero uncontrolled failures, 679 controlled busy replies, P95 1.051 seconds,
zero health errors and five controlled health busy responses. Uptime remained
continuous, sensors valid, and recovery took 0.118 seconds. The subsequent 8-client regression also passed: 944 HTTP 200 responses, zero
failures or refusals, P95 0.565 seconds, zero health errors, recovery 0.118
seconds, valid sensors and continuous uptime. Evidence:
`build/evidence/m1-qa/r15-validation-16x120/`. This is a short-load success,
not long-run acceptance or proof of the proposed close/reset mechanism.

The R16 source adds the missing M1 counter serialization and ports the matching
bounded accept/drain path to A1. Both accept/request-worker implementations
match after product-name normalization. Frozen source hashes:
M1 `57f4fe532f513ec447a7c6294ca181faaa196e9ec4d6a4fecf42984fabbebaf2`,
A1 `13e53ac3f89ca7909b66e8ac0c9a03ffc2abdcb0b42b66b605f2f1f099de99de`.
A1's local build left a stale signed artifact whose payload did not match the
new raw image. Offline validation rejected it and no A1 upload occurred. Its
candidate was then freshly signed and validated by the existing trusted signing
workflow; build/checksum success alone is not deployability evidence. Final A1
signed SHA-256 is
`9f2972e540abee27c7f9f9667665d79a4fc665b6a74d73d057b211434cd6441f`;
M1 R16 signed SHA-256 is
`63be37a9e8b2d0b31d37d8d38e9c018902a5858b32286f6122e9e7668fe9c145`.
R16 signed deployment has completed on M1 (HTTP 202), with queue capacity 8,
TCP requested/readback 24, the drain counter present, valid sensors and retained
configuration. Final device load acceptance remains in progress.

The A1/M1 packaging scripts now invalidate only their own old signed OTA file
when producing a fresh package. A regression verifies stale-signature removal,
preservation of unrelated/product-external files, and raw/manifest/checksum
consistency for both products. It passed via
`python3 -m unittest tests.test_package_stale_signature`; evidence is in
`build/evidence/packaging-stale-signature/`.

The fleet simulator fault-retry matrix passed 23 cycles, with at least 20
injections per fault type; wrong-token, stale, and abnormal-sample packet counts
were 20, 31, and 24. Maximum LAN recovery was 6.014 seconds and seven relays
remained normal. The simulator exercises production service, automation,
binding, grouping, engine, and MQTT workers over loopback with a dedicated test
broker; it does not prove physical multi-device timing. Evidence is in
`build/evidence/fleet-r7-run-20261015-181857-fault-retry.log` and
`build/evidence/fleet/scenarios.ndjson`.

`bash tests/run.sh` and `SANITIZE=1 bash tests/run.sh` passed, including
ASan/UBSan host regressions. A1 and M1 ARM builds passed. The final R16 ordinary and ASan/UBSan host runs both exited zero; logs are in
`build/evidence/r16-host-tests/`. Independent A1 and M1/common source reviews
found no confirmed additional defects. The A1 SHA-256 was independently checked
against the frozen value; a reviewer initially compared it with a Git blob SHA-1,
which is a different hash type. These checks do not replace device acceptance
or the longer soaks below.

## Acceptance still to complete

Resolve and retest the M1 simultaneous-start resets, then record final
candidate hashes, 1/4-client 10-minute and 8/16-client 2-minute results, full
4-hour mixed-load evidence, idle heap medians, shared linkage/fault matrix,
original configuration restoration, clean final build/deployment smoke, and
required current-head CI before merging. Any runtime fix restarts the full soak.


## R16 repeated-load rejection (2026-10-16)

Both signed images deployed successfully, but neither passed final repeated load.
A1 stopped after approximately 75.25 seconds of its requested 120-second first
16-client round: 1136 requests, 388 controlled JSON 503 busy replies, zero
connection resets, and three HTTP 200 status bodies with invalid JSON. Each
saved invalid body is 1919 bytes and ends with incomplete/corrupt JSON near
`save_error`. Recovery completed in approximately 0.16 seconds with continuous
uptime and healthy Wi-Fi/MQTT/panel/fan. An independent harness defect also
classified a binding health probe's controlled 503 as fatal; that classification
has been corrected locally, but does not invalidate the three real bad bodies.
Evidence: `build/evidence/a1-qa/r16-16-r1-run2/`.

M1 stopped after approximately 1.803 seconds of its first 16-client round:
32 requests, 17 HTTP 200, 12 controlled busy replies and three connection resets.
Health samples had no errors, uptime was continuous, and recovery took 0.105
seconds. Counters advanced by 32 accepted, zero accept failures, 20 queued,
12 rejected, 19 positive drain reads and 12 successful busy writes/closes.
The earlier R15 successes therefore do not establish a reliable reset fix.
Evidence: `build/evidence/m1-qa/r16-validation-16x120-r1/`.

A source defect in the pinned SDK json-c can return partial JSON after printbuf
reallocation failure because nested serializers ignore append failures. A new
sticky-error patch makes top-level serialization fail rather than return a
partial document, resets the error for retries, and frees the standalone buffer
on `_ex` serialization failure. Deterministic allocation-failure regression and
review are in progress. This defect is consistent with the A1 samples but has
not yet been proven to be their device-level root cause. No further final-load
or soak result is claimed; both devices recovered and were handed back before
additional diagnostic work.


The next R17 candidate additionally tests a bounded overload-response close:
send the complete 503 first, then read/discard until EOF or one shared absolute
100-ms deadline (maximum 64 reads). No rejected request is executed. This avoids
using the SDK's ambiguous shutdown constants; it remains best effort for slow
or continuously sending clients, and target select timeout behavior needs live
proof. Complete sends alone increment send-ok. This pre-backlog R17 draft was superseded before deployment. Its HTTP source
SHA-256 values (not final R17) were:
A1 `c40573661812ae4a3dfd869b3216031db67f92ec6d051b71cb80daf50aaf8097`,
M1 `a13c31e3e634df4458935505a79e87558f8912a2ca3bbb1e81df2dfa6d4e4689`.


R16 targeted M1 capture reproduced five resets in a 16-request diagnostic
(1.079 seconds), alongside eight HTTP 200 and three complete controlled 503
responses. All failed flows completed a handshake and transmitted their request;
no response preceded the device-IP RST. Counter deltas were accept +14, queue
+11, reject +3, with the three rejects matching the three complete 503 flows.
These observations do not support assigning all five failures to the busy close
path. No Ethernet source MAC is available in this cooked capture. Device uptime
remained continuous and recovery took 0.131 seconds. Evidence:
`build/evidence/m1-qa/r16-reset-diag-run/pcap-analysis.md` (335 packets, zero drops).

The listener still used `listen(listener, 8)` independently of the configured
TCP limit 24. R17 additionally requests backlog 24 while leaving the application
queue at 8. This tests a pending-accept capacity hypothesis; the closed SDK may
interpret or cap the requested value, so the change is not root-cause proof.
The earlier pre-backlog R17 raw builds are not deployable final candidates.
Final R17 HTTP source SHA-256 values are now:
A1 `8cccf84aa541fc44df858a9aa108623f8f01254d05a59a1a9275302960587232`,
M1 `35ae8ab34b752eff3ebaa0767c306782ccf03a7d947200b08f22e9e74206bdd5`.
The JSON allocation-failure regression passed normal and sanitizer host suites;
a fake-socket test also passed for the actual extracted overload branches,
covering partial writes, timeout, EOF, read limit and clock wraparound. These
host checks do not establish target socket timing or device-level resolution.


Final R17 ARM builds completed for both products. HTTP accept static frames are
192 bytes plus the configured 1536-byte reserve (1728/2048); this is a static
budget, not a runtime watermark. Both SDK caches have the printbuf patch and
all three ABI-dependent JSON objects were rebuilt before the final backlog
incremental build. Raw image SHA-256 values:
A1 `a4047e0efcaea14ddb9549a8937fa79cf02465d9d57fe589e1df73aed8f2def1`
(650076 bytes),
M1 `fdd115d7509fe2c5c61c32d0d3f516fda0267a597a268a716533d4d1422bd353`
(627476 bytes). Manifest/target/checksum checks passed; signing and deployment
are separate pending gates. Evidence: `build/evidence/{a1,m1}-r17-build/`.
The final ordinary host suite passed with both already-patched and unpatched
SDK input, and the final ASan/UBSan suite exited zero, including extracted HTTP
branch and JSON OOM regressions. Logs:
`build/evidence/json-printbuf-patch-host/`. No four-hour acceptance is claimed.


R17 signed-image validation and signature regression passed (7/7). Signed
SHA-256: A1 `c298bbd0f3bc5ed18e8ee1d4fcf169fd9b25c71d1afe482f3fbee922d8fd1b2f`,
M1 `467f4babd4f014fa0366f867661aead76f1d4ddcb8db24a9f3bb71f4e41cae5d`.
M1 received one signed OTA (202), restarted with its expected identity and
retained configuration; Wi-Fi/MQTT/sensors were ready, queue 8/TCP readback24,
set/get rc0 and drain counter present. Final repeated-load testing is underway;
deployment success is not an acceptance result.


R17 failed final load on both DUTs. M1 stopped at 1.016 seconds/16 requests:
nine HTTP 200, six controlled 503 and one reset; API/sensor recovery took 0.115
seconds, uptime continuous, health errors zero. A1 stopped around 41.447
seconds/464 requests with 100 controlled 503 and 20 uncontrolled failures
(two reset/disconnect, eighteen timeouts), P95 4.327 seconds, and an observed
uptime rollback from pre-run 29378 ms to 5250 ms. Its counters reset and their
cross-reboot deltas are explicitly invalid. Both recovered; no later matrix
stages or soak were started. Evidence: `build/evidence/m1-qa/r17-validation-16x120-r1/`
and `build/evidence/a1-qa/r17-16-r1/`. R17 is not accepted.

Further SDK inspection found confirmed allocation-failure defects: linkhash
allocation failure calls exit(1), full tables grow by only one entry per insert,
and string constructors omit checks before using allocation results. These are
source defects, not yet proof of A1's reboot cause. A separate allocation-safety
overlay and deterministic failure-injection tests are being prepared before any
new candidate; MOC's actual global connection capacity remains unproven.


Functional harness restoration was strengthened locally before further DUT
mutations. Both skip auth changes when an existing web password is enabled;
otherwise cleanup is registered before sending a temporary-password mutation,
including a lost-reply path. Independent cleanup steps are attempted even if an
earlier step fails, followed by owned-state readback (config/time/night/schedules,
M1 brightness, A1 light/fan/child-lock/buttons and bindings as applicable). M1's
original schedule ID 2 is explicitly retained. Offline mocks and py_compile
passed; this is harness validation, not another physical functional run.

Post-R17 source diagnosis kept the two observed failures separate. A1's 19724-byte
heap sample has uptime 5250 ms and MQTT `connecting`: it is after the reboot,
not a pre-crash allocator watermark. Neither the recorded sampled minimum nor
the confirmed JSON allocation defects establish this reboot's cause. M1's
R17 `/api/v1/logs` reset occurred with continuous uptime; accept/queue deltas
include health probes, so their totals cannot identify the failed flow.

The next source candidate adds only a boot-cause diagnostic to the shared status
producer: `last_reset_reason` is the raw SDK integer, read once per boot, guarded
by `MOC=1` and `CONFIG_CPU_MX1290`. The pinned A1/M1 generated configs compile
`platform/MCU/MX1290/moc/moc_api.c`; that source defines
`int get_last_reset_reason(void)` using `int (*last_reset_reason)(void)`. Both
existing `Lib_MiCO_Kernel.a` archives expose the symbol. The original MK3080B
board startup also uses that exact function via an extern declaration. The SDK
defines flag bits 0..5 as VBAT, AV12, AV18, software reset, lockup, and watchdog.
The API field preserves combined/unknown bits; a watchdog flag alone does not
distinguish faults, starvation, or deadlocks. Actual device readout remains
unverified, and no M1 connection-capacity change is justified by these data.

The production status-producer regression runs against the patched SDK JSON
library in portable and MX1290/MOC modes. It checks raw combined boot flags,
single-read caching, availability without heap telemetry, sampled heap minima,
and tick rollover. The portable executable links without any reset-cause SDK
symbol. Host normal and ASan/UBSan evidence is retained under
`build/evidence/r17-reset-diagnostic/`. No new firmware build, deployment or
physical load test was performed for this diagnostic candidate.

Follow-up OTA regression found a missing direct include: the shared
`mqtt_ota.inc` uses the checked JSON producer wrappers but its standalone test
only includes `json.h`. The fragment now includes `../json_build.h` itself;
the OTA harness treats implicit function declarations as errors. The complete
OTA suite passes, including both seven-case signed verifier variants, MQTT
OTA behavior, and seven TC1 adapter cases. A1/M1/TC1 native MQTT source syntax
checks also pass with host platform stubs and the implicit-declaration error
enabled; this is not a new ARM firmware build.

A separate confirmed protocol OOM defect was fixed at the shared MQTT enqueue
and UDP send boundaries: absent envelope objects are rejected before JSON
serialization. `group_envelope` also returns immediately if its root allocation
fails, and `group_send` rejects an absent envelope. These guards preserve the
SDK's valid `json_object_to_json_string(NULL)` representation while preventing
failed group allocations from sending that literal as a protocol message.
Deterministic constructor failures cover group controls/configuration over LAN
and MQTT, and LAN recovery probes, asserting no queued MQTT message or received
UDP datagram. Normal and ASan/UBSan host suites pass. Evidence is under
`build/evidence/ota-json-build-include/`; these defects do not establish the
cause of the earlier A1 reboot or M1 connection reset. No DUT access occurred.

The next repeated A1/M1 build exposed an overlay idempotency defect: later JSON
overlays alter the context of `json-newlib-reent.patch`, so checking that earlier
patch in isolation cannot recognize the final SDK state. Both build scripts now
pass the three JSON overlays to `apply_sdk_patch.py` as an ordered stack. The
tool copies only affected SDK files, tries reversing each possible complete
prefix in reverse order, then replays the whole stack. Only a fully valid result
is written back, and byte-identical files are left alone. Unrelated SDK changes
remain intact; an incomplete or incompatible stack fails without SDK writes.

Five apply-tool regressions pass. Real pinned A1/M1 JSON sources cover all four
prefixes (zero through three applied layers), repeated final-state application,
preservation of unrelated source edits, and rejection without writes when the
new reentrant helper header is missing or modified. Existing nested archive,
SDK git checkout, and LF/CRLF cases remain covered. Replaying the full stack on
both existing SDKs left all affected JSON bytes unchanged. Complete
`bash A1/build.sh` and `bash M1/build.sh` then passed, including stack budgets,
image metadata and JSON decimal linkage checks. Logs and before/after hashes
are under `build/evidence/sdk-json-stack-idempotency/`; generated artifacts are
under `build/a1-release/` and `build/m1-release/`. These builds were not signed
or deployed, and the earlier physical failure causes remain unproven.

R18 A1 functional validation twice observed a bare HTTP 503 `busy`: once on
fan-30 status readback and once while polling scheduled execution. Both runs
restored owned state successfully. Accept queue rejection and busy-send
counters remained zero, uptime stayed continuous, and MQTT/panel readiness
remained healthy. The second preceding status sampled 18372 free bytes and a
13704-byte observed minimum. These responses match the JSON construction or
serialization failure fallback, not an ordinary status-query busy policy.
The failed allocation and the instantaneous heap state remain unknown.

Source inspection found a concrete avoidable peak: A1 HTTP JSON responses and
shared MQTT state/result publication retained their entire JSON object graphs
through synchronous socket sends or PUBACK waits. They now serialize to an
independently owned SDK printbuf and release the graphs before network waits;
only wire bytes survive until send completion. No extra string copy is needed
for a freshly built status. This reduces overlap with the next status producer;
it does not establish a permanent leak or prove that this caused either 503.

New host regressions execute A1's production response functions and shared MQTT
publication, observe root/child frees before the send callback, validate wire
content after those frees, and cover constructor/serialization and send
failures. Existing nested MQTT callback queue behavior remains covered. Normal
and ASan/UBSan host suites pass; A1/M1 complete firmware builds and A1/M1/TC1
native MQTT syntax checks also pass. Evidence and build manifest snapshots are
under `build/evidence/r18-json-wire-lifetime/`. No deployment or physical retest
was performed; the two functional runs remain failures.

Final host and OTA regression suites were rerun after those builds. Normal and
ASan/UBSan `tests/run.sh` and `tests/ota/run.sh` all passed; logs and SHA-256
digests are in `build/evidence/r18-final-host-normal.log`,
`build/evidence/r18-final-host-sanitize.log`, and
`build/evidence/r18-final-ota.log`. Both release directories pass their
`SHA256SUMS` checks. The A1 OTA payload is 650652 bytes
(`cfe25401086f8cda64fc83d35801ea05cd164526ce35f8a74d5ba4c6e029c966`); M1 is
628084 bytes (`93dc444c30664b1a04645de8875721c3ee26962450c22a7eb2430ea4fde99138`).
The final R18 simulator rerun completed successfully against the final group and
service sources with AddressSanitizer and UndefinedBehaviorSanitizer enabled
(leak detection was disabled for the long-running fleet). Fourteen processes
(seven A1 and seven M1) completed 49 LAN bindings; control scenarios passed;
group coordination completed 20 takeovers and rejoins; and 23 injected-fault
windows all recovered, with at least 20 confirmed injections in every fault
mode. All process stderr logs were empty and all simulator/proxy processes were
stopped afterward. The run summary, per-stage logs, event records, source
hashes and binary hashes are under `build/evidence/r18-final-simulator/`.
These are isolated software/network simulations and do not replace DUT load,
soak or physical-function testing. No signing key was found in the current
environment, so at that point the R18 images remained unsigned and physical
acceptance and merge remained blocked. The physical test update below records
the subsequent explicitly authorized unsigned deployment and DUT results.

## Physical candidate test update (2026-10-16)

The R18 JSON wire-lifetime candidate was deployed to both test devices using
the explicitly authorized unsigned OTA endpoint. The deploy helper still
validated each native image's target, format, checksum and manifest identity;
publisher signature verification was skipped. A1 `b0f893303134` runs raw image
SHA-256 `e8c3b86dcacbe9f2059c4e3ec70fb23e8ce15e356bd469edd9b5ec4f688a6ff8`;
M1 `b0f8932864b6` runs `bf0764a7eadf2744381476d5babafc509bca82492a1d48c4965df2b3fc09b2a6`.
Deployment records are under
`build/evidence/r18-json-wire-lifetime-deploy/{a1,m1}/`.

Both devices passed functional validation and 1-client/600-second and
4-client/600-second load stages. A1 recorded 591 and 2216 requests in those
stages, with zero failures, P95 0.4203 and 1.6105 seconds, and continuous
uptime. M1 recorded 596 and 2193 requests, with zero failures, P95 0.4000 and
1.7657 seconds, continuous uptime, and no invalid sensor samples. Evidence is
under `build/evidence/{a1,m1}-qa/r18-json-wire-lifetime/`.

At 8 clients for 120 seconds, A1 completed 896 requests with four expected,
protocol-level `503 busy` responses and no uncontrolled failures; recovery
passed in 0.148 seconds. M1 completed 894 requests with no failures and
recovered in 0.106 seconds. At 16 clients for 120 seconds, M1 completed 1752
requests with 634 expected controlled refusals, no failed requests, continuous
uptime, valid sensors and 0.092-second recovery.

A1's 16-client run failed after 45.586 seconds and 475 requests. It recorded
142 controlled `503 busy` responses, then 16 request timeouts and 29
`EHOSTUNREACH` errors; no TCP reset was observed. The monitor and the
30-second recovery window also ended with `EHOSTUNREACH`, so there was no
recovery sample and reboot continuity cannot be inferred. The last successful
status read had 26,532 free bytes, 9,108 minimum observed free bytes, queue
reject/send counters of 90/90, and Wi-Fi/MQTT ready. This does not distinguish
an A1 network-stack failure from an AP/router path failure. A1 remains out of
further load testing while unreachable; do not interpret its 16-client stage
as a pass. Sol's read-only evidence review found M1
continued answering on the same test subnet during A1's outage; the host route,
gateway neighbor and physical host link snapshots showed no corresponding
failure, but there was no contemporaneous packet capture, AP association/ARP
history, UART or post-failure A1 status. The recovery field
`uptime_continuous=true` had zero samples and is not evidence that A1 stayed up.
A separately bounded single read-only status query later timed out after three
seconds, so the device remains unreachable. Detailed analysis and hashed input
references are in `build/evidence/a1-r18-16-offline-analysis/analysis.json`;
no firmware change or additional load was made during that investigation.

The source review found no provable cause for the outage and no deterministic
fd leak or application-level deadlock in the queue-rejection path. It did find
that the pinned MiCO SDK documents listener sockets as thread-affine, while the
previous implementation queried the listener from both accept and request
threads; it also found normal response sends lacked one total time bound. A
follow-up A1-only candidate now creates/configures/accepts/diagnoses the
listener in its owner thread and sends each response with a shared 10-second
nonblocking deadline. The ineffective SDK `listen` backlog parameter is now
passed as zero; `TCP_MAX_CONN_NUM` remains set and read back as 24. Focused
normal/sanitizer tests, full normal/sanitizer host suites, A1 build and stack
check pass. Candidate payload SHA-256 is
`38221c4c6a41af1676d126c9e967906c794e41f61b57b6329af8d1d5bdbaad42`; evidence
is under `build/evidence/r18-a1-http-owner-deadline/`. This is an SDK contract
and bounded-blocking risk fix, not a proven fix for the observed offline event.
It has not been deployed because A1 is unreachable, so it has no physical
validation yet.

M1's four-client, four-hour mixed-read soak was interrupted after about 14
minutes when its evidence was reviewed: 838 request records contained nine
uncontrolled failures (six `503 busy`, three five-second timeouts), and 163
health records contained one timeout. The scripted soak did not fail fast and
continued recording for roughly eight minutes after the failure window before
the runner was terminated; later health samples recovered, but there is no
completed soak summary and this stage is a failure. Logs are
`build/evidence/m1-qa/r18-json-wire-lifetime/soak-requests.ndjson` and
`soak-health.ndjson`. Sol's read-only analysis at
`build/evidence/m1-r18-soak-failure-analysis/analysis.json` confirms the
queue-reject, busy-send and overflow-close counters each increased by six,
matching the six `503 busy` responses. All 162 valid status samples had
monotonically increasing uptime; sampled heap minimum, MQTT and UART remained
stable around the event. The accept count also rose materially more than the
logged soak requests in two adjacent windows, so the device was receiving
additional traffic not attributable to this runner. Current local process
inspection found no device runner, while the active headless browsers were
started after the failure window; there is no time-matched process snapshot or
packet capture to identify the extra source or prove it was absent. Sol found
no provable firmware root cause, but identified the same listener thread
ownership and unbounded normal-response send risks as A1. A M1-only candidate
now keeps listener setup, accept and listener diagnostics in one owner thread,
sets the ineffective SDK backlog argument to zero, and bounds normal response
header/body transmission to a shared 10-second nonblocking deadline. Focused
normal and ASan/UBSan tests, full normal and sanitizer host suites, the M1
build and static stack check all passed. The accept frame is 216 bytes plus a
1536-byte configured reserve within the 2048-byte budget; this is static
analysis, not a runtime stack watermark. Candidate OTA SHA-256 is
`62f092d41dfadc000df902d9e437d7d043a322e91fa184763fa3a662946affac`; check
logs and source hashes are in
`build/evidence/r18-m1-http-owner-deadline/results.json`. The historical-input
audit at `build/evidence/r18-m1-http-owner-deadline/historical-input-audit.json`
confirms the original failure analysis and all non-HTTP inputs remain
unchanged. The candidate changes mitigate SDK ownership and blocking risks,
but do not prove the original soak root cause. After the host checks completed,
the M1 candidate was deployed under the existing unsigned-deployment
authorization. Device ID,
candidate hash/version, OTA restart marker, preserved configuration and
peripherals were verified; deployment evidence is in
`build/evidence/r18-m1-http-owner-deadline/deploy/`. Functional validation
passed, including sensor, display, schedules, NTP fallback, reboot persistence,
Wi-Fi scan and temporary-auth cleanup. The new 1-client/600-second run passed
592 requests with zero failures, P95 0.470 seconds and 0.117-second recovery;
the 4-client/600-second run passed 2346 requests with zero failures, P95 1.116
seconds and 0.128-second recovery. Both had continuous uptime and no health or
sensor errors. Evidence is in
`build/evidence/m1-qa/r18-http-owner-deadline/`.

The next 8-client stage failed after 1.75 seconds: 16 requests included one
TCP reset on `/api/v1/schedules`. The only health sample was timestamped about
0.925 seconds before the reset; it showed 37128 free bytes, accept and queue-ok
counters of 3200, zero queue rejects and nine TCP connections, but does not
establish the counters at the reset itself. The request log's source-port field
can retain the previous successful request's port after an open failure and
does not identify this reset's connection. Recovery passed in 0.187 seconds,
but this is still a failed load stage. The 16-client stage and soak were not
run. Evidence is in `load-8-first-failure.json`,
`load-8-summary.json`, `load-8-requests.ndjson` and `load-8-health.ndjson` in
the evidence directory above. Sol reviewed and hash-bound 44 source, build,
deployment and test inputs in
`build/evidence/m1-r18-owner-deadline-load8-analysis/analysis.json`
(SHA-256 `1232e724324ee1cd9c559a23d733ae2116e7583150b30ba06099960e14d7f591`).
The analysis found no provable production root cause. A client-port mock
confirmed the test client's logged source port can be stale after a pre-header
failure. Full focused and host sanitizer suites and an M1 rebuild passed with
unchanged source and OTA hash. Sol recommends keeping ordinary 8/16-client
acceptance and soak paused until a short packet-capture diagnostic records the
actual flow tuple and event-time state. Its plan is saved at
`build/evidence/m1-r18-owner-deadline-load8-analysis/capture-plan.md`.

The capture attempt could not proceed: this host's noninteractive sudo policy
allows interface listing but requires a password to open a packet capture.
The initial tcpdump permission failures happened before any diagnostic load;
one read-only status request was used to finish a bounded one-packet preflight,
and M1 answered with the expected ID. No diagnostic pcap or event-time counters
were collected, and no further device requests are being made. The normal
response deadline does not bound request parsing, scheduler delay or SDK close
duration. Physical acceptance and merge remain blocked on A1 recovery, a
captured M1 reset diagnosis and retest, the remaining load matrix and a clean
M1 soak rerun. No signing key or publisher signature was used for these device
images.

### Real TCP simulation follow-up (2026-10-16)

The existing fleet simulator builds product service/MQTT code but not the
physical HTTP server. Its prior passes therefore do not cover the failed HTTP
connection path. New `tests/test_http_tcp_integration.py` and
`tests/http_tcp_harness.c` extract the current A1/M1 request parser, response
transport and accept/request workers into executable C using real Linux
loopback TCP. Pthread queues/threads, listener binding/backlog and MiCO-only TCP
options are host adaptations; the route handler is a test adapter. Product
service JSON, OTA, Wi-Fi, MQTT and the proprietary MOC networking implementation
are outside this experiment.

All eight scenario tests passed normally, with ASan/UBSan, and in an independent
6-Luna normal rerun (1.719 seconds). Evidence is under
`build/evidence/r18-http-tcp-simulation/{normal,sanitizer,luna-independent}/`.
The new suite is included in `tests/run.sh`; the complete host suite also
passed (`build/evidence/r18-http-tcp-simulation/full-host.log`).
The results distinguish the following hypotheses:

- Each product completed 204 ordinary GET requests in repeated 9/8-client
  bursts without unexpected resets. Controlled queue-full responses were
  accounted for separately.
- A delayed reader received the complete 65536-byte body even when output was
  still pending at nonblocking close. Restoring blocking mode before close also
  succeeded. This does not reproduce the physical reset or prove MiCO close
  semantics.
- A GET body left unread at close, or an overlong request rejected with unread
  bytes, produced a raw-client-visible reset. Restoring blocking mode did not
  prevent it; experimental draining did. A Content-Length-aware HTTP client
  could nevertheless receive the complete response successfully. These cases
  do not match the historical ordinary, bodyless GET failure.
- With one held request and eight queued clients, overflow returned a complete
  503 and all tracked accepted descriptors were closed. Actual send
  backpressure terminated within the experimental 350ms response deadline and
  a subsequent request succeeded. The shorter deadline is explicitly a test
  variant, not a production configuration change.

The same pinned SDK's configuration server also hands accepted child sockets
to a new worker thread (`MiCO/system/config_server/config_server.c`), so that
ownership transfer alone is not evidence of an SDK contract violation. Its
MOC socket source delegates to a binary API table; Linux results cannot prove
the underlying device networking behavior. Tracked adapter allocations and
descriptors returned to zero, but that is not a whole-firmware leak check.

The device test client now clears `last_source_port` before each request so a
pre-header failure cannot inherit a previous request's port. A local mock
verified the fix without device access; evidence is in
`build/evidence/r18-http-client-port-reset/result.json`. This changes diagnostic
metadata only and cannot recover the historical failing flow tuple.

No physical device was accessed and no production firmware was changed or
deployed for this follow-up. The physical M1 reset, prior soak failures and A1
loss of reachability remain distinct unresolved observations; the simulation
does not establish a common root cause or satisfy the physical merge gates.
