# UDP local pairing acceptance

Local password pairing and MQTT-issued sensor grants are independent
authorization sources. A broker connection is not a prerequisite for local
pairing. A1, M1, TC1 and Daikin must use the same MessagePack records; direct
peers upgrade together. No JSON UDP fallback is provided.

## Contract

- `auth_pair` authenticates the target's configured device password. An unset
  password is an empty string. A durable peer key is exchanged by
  `auth_resume` for a boot-local scoped token. Durable keys are not data tokens.
- Pairing responses must match the pending peer, remote IP, operation and
  16-byte nonce. Secrets use MessagePack binary values with explicit lengths.
- Incoming authorization records and outbound peer credentials are separate.
  Asynchronous send/retry jobs own their input; request storage is borrowed
  only for synchronous parsing and dispatch.
- Authorized sensor datagrams use LAN v3 and the shared `lan_wire.h` typed
  representation. MQTT sensor-values-v3 metadata/readings is a different
  envelope; it must not be substituted as a UDP sample body.
- Password replacement, restore and reset must revoke local pairing through
  a durable transaction. Failed storage must not acknowledge successful
  revocation or leave RAM and persistent configuration inconsistent.

## Required evidence

These are acceptance requirements, not a statement that the tests have passed.
Host tests must execute production adapters and shared core functions. Device
interoperability is recorded separately after flashing exact candidate images.

| Scenario | Required result |
| --- | --- |
| Empty password; configured password; incorrect password | First two pair successfully; incorrect password grants no key/token |
| Pair, resume, read/probe in both directions | Exact shared MP envelope; no string truncation or format fallback |
| Wrong peer/IP/op/nonce; malformed or oversized binary secret | No pending-job completion or authorization |
| CONTROL / SENSOR / GROUP scope separation | A token cannot authorize an unrelated operation |
| Reboot with persisted peer keys | Resume creates a new boot-local token; old token is rejected |
| Full peer/session tables | Explicit failure without evicting unrelated active peers |
| Remove A from A/B/C, resume C, then unpair C | All C sessions rejected; B remains authorized, including after reboot |
| Change password, restore, reset; inject persistence failure | Successful commit revokes required credentials; failed commit preserves a coherent recoverable state |
| Duplicate/late reply, retry, epoch change during pairing | No stale completion, credential resurrection or borrowed-buffer access |
| MQTT grant with no local pair; local pair with MQTT disconnected | Each authorization source operates independently |
| Real MiCO encode -> Daikin decode/reply -> MiCO decode | LAN v3, binary token, uint32 counters and declared measurement units match |
| Missing/invalid/stale sensor fields and boot/sequence replay | No synthesized valid zero or renewed stale lease |

Release remains blocked until this matrix, full/hub target builds, and required
post-flash checks have evidence. Passing host vectors does not prove live
device interoperability or physical behavior.
