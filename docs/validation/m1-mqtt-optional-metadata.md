# M1 core subscriptions and optional sensor descriptors

The deployed broker accepts `sensor/+/state` and rejects the broader
`sensor/+/+` and the separate `sensor/+/metadata` subscription with SUBACK 128.
M1 now keeps state in its mandatory subscription plan and requests metadata
independently. Only the validated, correlated `MQTT_SUBSCRIBE_DENIED` result is
nonfatal; malformed ACKs, wrong packet IDs, timeouts and transport errors close
the session. The existing conditional source-availability subscription remains
mandatory. Native commands and core READY remain available after a metadata
refusal. The diagnostic identifies the optional metadata subscription.

Only denied metadata is retried, at most once per 30 seconds on the separate
MQTT owner thread. A granted subscription is not periodically requested again.
The two owner-lifetime filters occupy 155 + 158 = 313 bytes, saving 199 bytes
against the previous single 512-byte filter, including the maximum username.
No shared Client or owner structure gains a field.

Retained descriptors pass the real ingress gate, while retained measurements
and controls remain rejected. Schema 3 parsing and device/boot/revision matching
remain strict; missing descriptors leave their dependent external display
unavailable. JSON is confined to the separate HA command path.

MQTT also permits PUBLISH before SUBACK. During subscribe, the shared adapter
temporarily routes the pinned SDK's default handler into its existing bounded
copy queue. Business dispatch remains outside protocol depth. Success commits
the route; failure restores the previous default handler and removes queued
input only for the newly attempted subscription index. Other route order,
in-flight entries, outgoing telemetry and receipt credit are preserved. Detached
allocations are released outside the core lock with admissions closed. A denied
early descriptor cannot survive into a later granted retry.

Validation used MiCO SDK commit `6c465211d3ff8797cd835e400ec54a06530dd476`
with the production overlay stack, real Paho serialization/read loop, shared
client/session code and extracted M1 callbacks. The 38-test ASan/UBSan run covers
26 M1 session cases, core accounting, concurrent clear, dual owners, Paho,
birth/optional JSON behavior and both products' HA gates. Negative controls
restore the broad filter, old retained gate, old post-SUBACK route and old
unconditional availability filter and must fail runtime assertions.

Six stack-gate tests additionally require the rollback frame and its owner tail,
and reject missing frames or exceeded original budgets. M1 initial/retry
subscription paths are included explicitly. SDK/RTOS reserves and all product
thread stack sizes are unchanged. Final complete SDK builds and DUT performance
acceptance are separate integration gates; this change does not deploy cloud
services or write a DUT.

Evidence: [ASan/UBSan and gate output](evidence/rc4-m1-optional-metadata.log).
