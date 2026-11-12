# MessagePack local authentication

New firmware speaks MessagePack on `/api/v1/*` structured routes. Assets and
`GET /api/v1/info` are public. Info contains only canonical identity, wire
capabilities, `auth: bearer`, and `password_set`; it never exposes Wi-Fi or broker
settings. OTA image and PHCBACK1 backup bodies remain opaque binary bytes.

When no management password is set, local HTTP requests pass directly. The UI
does not prompt for a password. `POST /api/v1/auth/login` accepts
`{password: ''}` and returns `{code: 'ok', data: {password_required: false,
token: ''}}`, without allocating a session or reading the random source.

With a password set, login accepts `{password: string}` and returns
`{code: 'ok', data: {password_required: true, token: '32 lowercase hex digits'}}`.
A wrong password returns 401/`unauthorized`. Four occupied HTTP slots or random
source failure returns 503/`busy`, without evicting an active session.
`POST /api/v1/auth/logout` accepts an empty map with `Authorization: Bearer …`.
Protected requests accept a valid Bearer token or `admin` Basic credentials.
Basic authentication does not allocate a token slot. Structured HTTP bodies,
including login, use MessagePack.

HTTP tokens are boot-local, expire after 30 minutes idle or 24 hours total, and
are invalidated by a committed password change, restore, or reset. Request bodies
are borrowed only during synchronous handling; UDP retries own their encoded
bytes. The HTTP owner captures authorization at admission and revalidates the
same context after waiting or receiving a body, and again under the business
lock before returning receipts or cached data or executing a command. Password changes and restore
advance the credential epoch, revoking previously admitted requests, including
Basic and requests admitted without a password. Restore revokes those contexts
even when it preserves the password.

## Persistent layout and recovery

A1 v10, M1 v11 and TC1 v12 configuration prefixes are frozen, including trailing
padding. A tagged version-1 `ph_auth_store` is appended after the exact old size.
It holds separate incoming pair records and outgoing remote secrets. An outgoing
secret never authorizes incoming packets. Existing configuration versions do not
change, and no new member is inserted into their old prefixes.

MiCO 3.6 CRC covers the whole parameter partition. Old erased/zero tails are
initialized as empty pair stores. TC1's separate legacy 3.5 CRC migration chain
also recognizes the exact frozen v12 size before its v11/v10 candidates. Unknown
or corrupt nonempty trailer schemas enter recovery; they are not silently reset
or persisted over. Full explicit reset remains available through recovery UI.

PHCBACK1 export retains the exact old prefix/length and does **not** export pair
secrets. Restore validates the old envelope, atomically commits that configuration
with an empty pair store, and revokes HTTP/local sessions even if its password is
unchanged. The original M1 v8/v9/v10 backup migration remains supported. Wi-Fi,
MQTT and unrelated product configuration survive ordinary auth updates.

Changing credentials, pair writes and restore serialize in the order
`auth mutex -> configuration mutex/reservation`. `ph_auth_credential_commit`
commits the replacement password and revoked store in one product persistence
transaction. Failed commits restore RAM and leave prior credential/pair/session
state intact. The SDK's indeterminate-commit recovery gate remains authoritative;
no route reports a skipped recovery write as a successful credential change.

An older firmware can still interpret the frozen prefix in a MiCO 3.6 partition.
It ignores the appended pair store and naturally cannot use local pairing. A
rollback followed by a password change must not be advertised as preserving
revocation on return to the new firmware: old firmware does not maintain the
trailer. Export a PHCBACK1 backup before rollback; restoring that backup in the
new firmware clears all local pairs. Physical upgrade/downgrade acceptance is a
separate test, not implied by layout/CRC host fixtures.

## LAN pairing and trust boundaries

LAN packets use version-3 MessagePack envelopes and explicit sensor/control
structures. Peers must upgrade together; no JSON translation is provided.
`auth_pair` requests contain `src`, `password`, `scopes`, and a random 16-byte
`nonce`. Allowed scopes are control=1, sensor=2, group=4. A correct local password
(including an empty configured password) creates a durable 16-byte key.
`auth_resume` exchanges that key for a boot-local token. `auth_unpair` requires
the peer's current local token. Responses echo nonce, operation and device source;
the sending state machine also verifies the configured IP and expected peer.
Only an independently copied request is retained for asynchronous retries.

Broker-authorized MQTT sensor grants are separate temporary sessions. Password
changes, local unpair, and PHCBACK1 restore never revoke those MQTT sessions.
HA compatibility remains JSON at its dedicated MQTT adapter; this auth layer
never converts application traffic through JSON objects.
