# A1/M1 stability acceptance cases

Owner: parent agent (design and acceptance). Implementation and test execution:
GPT-6-Luna workers. Work only in the isolated stability worktree. Existing failed
and interrupted runs remain evidence; they are never replaced by later passes.

## Release gates

1. Reproduce the MQTT malformed-PUBLISH parser failure against the pinned SDK.
   Reject truncated headers, invalid remaining-length encodings, out-of-bounds or
   empty topics, reserved QoS and incomplete/zero QoS packet IDs. Valid QoS 0/1/2
   and payload boundaries must still work. Run ASan/UBSan and shared TC1 regressions.
2. Freeze production source hashes after review. Build all three products; sign
   model-specific A1/M1 candidates, verify manifest/raw/signed digests, upload once
   to the correct identity, and prove restart, peripheral readiness and retained
   configuration. Uncertain upload outcome requires inspection, not blind retry.
3. Independently run A1/M1 functional cases and the load table below. HTTP 200
   alone is insufficient: parse complete JSON and validate API envelopes. Reject
   nonfinite numeric values. Preserve the first malformed body privately.
4. Exercise the final runtime fleet, the 16-node matrix and reversible faults.
   Finish shared configuration mutations before taking the soak baseline.
5. Run four continuous hours on both physical devices simultaneously, with the
   fleet active. Any production-code correction invalidates this final soak.
6. Commit only owned source/tests/docs, excluding credentials, node_modules
   symlinks, generated builds and private evidence. Clean final build, signature,
   OTA and 15-minute smoke must match the committed production source. Restore
   original broker/bindings and owned settings, retain original scheduled tasks.
7. Require the current PR head's protected checks before merging main. Record
   exact commit and merge result; never bypass branch protection.

## Physical load matrix

| Clients per device | Duration per stage | Acceptance |
| --- | --- | --- |
| 1 | 600 s | Zero failures, P95 <= 2 s, no reboot or malformed JSON |
| 4 | 600 s | Zero failures, P95 <= 2 s, no reboot or malformed JSON |
| 8 | 120 s | Controlled overload allowed; no reboot, corruption or bad JSON |
| 16 | 120 s | Controlled overload allowed; no reboot, corruption or bad JSON |

Request timeout is five seconds. Record counts, exact duration, latency and
failure classes. After each stage, verify normal API and model-specific peripheral
readiness within 30 seconds. An interrupted stage is incomplete. Connection
failures must be classified explicitly; they are not silently renamed controlled
application rejection. Retained allocations and temporary load memory are not
automatically leaks.

## Functional coverage

- A1: power/fan 0..100, RGB/brightness/effects/segments, repeated saves and stale
  revision handling, child lock, button mapping read/write/restore, logs/paging,
  weekly and one-shot scheduling, time/night settings, auth, scan and persistence.
- M1: valid/fresh sensor readings, brightness steps, invalid values, duplicate
  requests, NTP preferred/fallback/failure retention, schedules, auth, scan,
  reboot/config retention. Preserve the pre-existing schedule ID 2.
- Preserve prior A1 calibration: 31 effective ring positions in a 32-slot buffer,
  bottom at position 11 with neighbors 10/12, top split 26/27. This describes the
  verified mapping; it does not claim 32 physically verified LEDs.
- The user's initial-candidate observation confirms normal M1 display and A1
  keys/light strip. Distinguish that observation from API checks on later images.

## Linkage and faults

Use seven native nodes per model plus the two physical devices: eight A1 and
eight M1, with up to eight sensor peers per A1. Force MQTT for edges whose host
loopback addresses are unreachable from the DUT; prove physical-pair LAN and
host/DUT MQTT paths from live status. Test fan/config fanout, all aggregation
statistics, fixed/linear/PID controls, stale/invalid sample exclusion and twenty
coordinator takeover/rejoin cycles.

For offline, delay, drop, duplication, reordering, wrong token, stale age and
abnormal sensor values, perform at least twenty real packet injections per type.
Record packet counters and expected fault windows, assert invalid samples cannot
corrupt aggregation, and require fresh LAN recovery within 30 seconds. Restart
only the dedicated test broker twenty times and verify uptime continuity and
MQTT/data recovery. Never reset a DUT or interrupt OTA power.

## Four-hour acceptance

Each DUT uses four HTTP clients at approximately one aggregate request/second,
plus MQTT/linkage traffic, with health sampled every five seconds. Capture idle
heap before and after the run (at least 35 seconds; prefer 60). After warming up,
the final idle median may lose at most 1024 bytes, with no sustained downward
trend, reset, malformed responses or errors outside declared fault windows.
Sampled allocator minima are not exact allocator watermarks. Host sanitizer proof,
signed-image proof, device runtime proof and physical observations remain distinct.
