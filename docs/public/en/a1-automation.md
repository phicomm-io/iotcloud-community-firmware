# A1 automation, linkage and remote display

## Automatic control and linkage

Add the actual M1 / DAIKIN device ID and optional LAN IP in A1, establish an authorized session on the binding page, then choose metrics, on / off conditions and fan limits in automatic control. MQTT can establish a session; paired LAN readings use UDP. No browser needs to remain open. Selecting automatic mode does not force power on; the mode persists while off. Expired data uses the configured offline policy rather than becoming zero. Fresh-air variants have separate fan policies; ordinary A1 does not expose unimplemented hardware controls.

Schedules, countdowns and sensor linkage are separate features; see [A1 operation](a1-guide). The field below is the **main fan's currently applied output percentage**, not RPM or external fresh-air fan speed.

## Device IDs and field IDs

A `device_id` is the factory MAC as 12 lowercase hexadecimal digits, such as `aabbccddeeff`. Read your device's ID in device information / diagnostics or select it under the same MQTT account. Field IDs are case-sensitive; use the identifiers below.

| Field ID | Meaning | Unit |
| --- | --- | --- |
| `fan_speed_percent` | Applied main fan output | `%` |

## Supply readings to M1 over MQTT

1. Use the project's binary MQTT mode on both the source and M1, with the **same external broker and MQTT account**. The account's ACL must allow subscribing to its sensor topics. The same server with different accounts generally does not provide mutual discovery.
2. In M1 Settings → General → Device screen user readings, choose the formaldehyde or PM2.5 region, then another MQTT device as the source. Select the actual device and field. If missing, check that both versions support sensor descriptors and have published them.
3. Save the source, unit, display mode and lifetime, then select user readings. A configured double press can switch both regions together. The default lifetime is 20 seconds; expired or invalid data restores that region's original reading.
4. Close the browser and cloud platform. Publishing, subscribing and display continue while both devices and the broker run.

M1 has fixed three-digit segment displays and cannot show arbitrary unit text. The PM region is integer-only; the formaldehyde decimal point is fixed after its first digit. For W, V, A or ppm, hide the original legends and mg/m³ / µg/m³ units as appropriate. See [M1 display formats](m1-guide#v1-candidates-user-readings-and-pm25-smoothing).

## Protocol and troubleshooting

Sensor descriptors and readings use **MessagePack schema 3**, published through `metadata` and `state` respectively. Topics are `phicomm/v2/u_<account encoding>/sensor/<device_id>/metadata` and `/state`. Firmware derives the account encoding from the username; an empty username uses the local scope. Metadata is retained and readings are not. Boot identity, sequence, descriptor revision, validity and age prevent reuse of stale data. Diagnostic tools need MessagePack decoding support.

For an offline source, missing field, unit mismatch or expired sample, check source publications and M1 lifetime. Invalid or missing data is treated as unavailable. UDP pairing, web management authentication and broker authentication are independent. Developer interfaces are in the [openiot protocol documentation]({{PROTOCOLS_URL}}).
