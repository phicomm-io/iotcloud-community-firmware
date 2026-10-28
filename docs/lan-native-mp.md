# Native LAN MessagePack v3

This wire change requires coordinated upgrades of sensor and linked purifier peers.
There is no JSON receive fallback and no JSON-to-binary conversion on UDP.
Existing binary configuration and backup layouts are unchanged by this module.

Sensor datagrams have `v:3`, 12-character lowercase hexadecimal `src`/`dst`,
nonzero uint32 `boot`/`id`, `op` (`read`, `probe`, `sample`, `pong`), and
16-byte binary `token`. Only `sample` has `body`. It contains `boot`, `sequence`,
`fields` (bits 0..4), `co2_supported`, and five-element integer `values`/`ages`.
Values are temperature milli-Celsius, humidity milli-percent, PM2.5 milli-ug/m3,
formaldehyde micro-ug/m3, and CO2 ppm; ages are unsigned milliseconds.
Invalid/missing fields are represented by clear validity bits, not valid zeros.
The decoder copies the complete validated sensor record into a typed structure.

Purifier group messages add `group` and use `group_join`, `group_probe`,
`group_ack`, `group_config` (`domain`/`body`), or `group_control` (`kind`/`body`).
Bodies use the same native parameter views and business validators as HTTP/MQTT.
Borrowed views exist only during synchronous dispatch. Retry slots own complete
encoded packets; they do not hold pointers into receive buffers or config trees.
The pending slot is installed before UDP send, so an immediate ACK cannot be lost.
Timeouts clear LAN eligibility and independently deliver the pending packet over
MQTT. Successful mutation advances the per-domain sequence; rejected mutations
remain retryable. The four-entry retired boot window rejects recent boot replays.

`auth_pair`, `auth_resume`, and `auth_unpair` are handled before normal traffic by
the local-auth module. Its local tokens are checked independently of existing
MQTT-issued temporary sensor grants. This module never clears those grants on a
local password change. Pairing only authorizes the local receiving endpoint;
remote configuration must explicitly install the returned token.

Host checks compile the actual codec and group implementation with ASan/UBSan:

```
PHICOMM_WIRE_CORE=/path/to/shared/core python3 -m unittest tests.test_lan_wire tests.test_group_mp
```

Coverage includes independent ownership, full uint32 boot, truncated/extra bytes,
invalid CO2, successful duplicates, failed command retries, retired boots, UDP
failure fallback, timeout counters, and an ACK delivered synchronously inside
sendto. These fixtures do not establish target fit, device compatibility, physical
button latency, or live network performance; those require the integrated build
and device campaign.

Local pairing sender is an independent state machine: `lan.pair` copies peer,
unicast IP, password and scopes into RAM; `accepted` only means queued. Every
request carries a random 16-byte nonce. Responses must match peer, source IP,
operation and nonce. Pairing success is reported only after the remote key is
saved under the initiating local credential epoch and a resume returns a token.
Outbound remote keys live in separate records from inbound authorization keys.
A reboot resumes configured peers from the durable remote key. Passwords are
never persisted and are erased on pairing completion/failure. Three attempts
are spaced one second apart; failed automatic resumes wait one minute.

For sensors, each pending read records its actual token independently from the
MQTT-issued session token. Local-pair reads can operate without MQTT. Group
requests substitute the local token only on UDP and echo it in ACKs; their MQTT
fallback retains the group credential. Configured local group peers may probe
without waiting for an MQTT join exchange. Changing local credentials cancels
in-flight local pairing; it does not clear the independent MQTT sensor grant.

The broker-only sensor grant also uses MP v3: `sensor.session.set` parameters are
`{v:3,a1:lowercase-peer,boot:nonzero-uint32,token:bin16}`. There is no JSON grant
adapter. The MQTT adapter discards retained grant commands.
