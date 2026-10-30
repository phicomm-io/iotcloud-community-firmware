# TC1 automation, linkage and remote display

## Local power automation

**Development firmware:** The following power-condition rules require the matching development build and are not included in the current RC4 download. Remote display of the `power` field described below is already supported.

In Settings → Schedules, select a power condition, **above / below a threshold**, a continuous duration and an explicit on / off action. For example, below 5 W for 600 seconds turns the outlet off. A duration of `0` triggers on a fresh matching reading; this is not cumulative energy in Wh. The limits are 0–25,000 W and 0–604,800 whole seconds, with at most eight power rules.

Comparison is strictly above or below; equality does not match. A rule fires once per matching period and rearms only after an observed nonmatching condition. Missing samples or reboot restart unfinished timing; a brief invalid sample does not repeat an action that already fired. Busy or failed storage does not count as success; the action remains retryable. Physical controls take priority: after a manual action the rule waits for the condition to clear.

Rules execute locally without a browser, cloud service, MQTT or NTP. Checks run once a second in addition to metering latency; this is not a circuit breaker, overload protector or precision timer. Rules persist, but an in-progress timer does not survive reboot.

**The threshold is total strip power, not power for one outlet.** Target outlet 1–6 or all outlets. Local meter pulses are averaged over about five seconds independently of MQTT reconnect/report windows. Power rules occupy schedule entries; existing weekly / date schedules still need correct device time.

## Device IDs and field IDs

A `device_id` is the factory MAC as 12 lowercase hexadecimal digits, such as `aabbccddeeff`. Read your device's ID in device information / diagnostics or select it under the same MQTT account. Field IDs are case-sensitive; use the identifiers below.

| Field ID | Meaning | Unit |
| --- | --- | --- |
| `power` | Total strip power | `W` |

## Supply readings to M1 over MQTT

1. Use the project's binary MQTT mode on both the source and M1, with the **same external broker and MQTT account**. The account's ACL must allow subscribing to its sensor topics. The same server with different accounts generally does not provide mutual discovery.
2. In M1 Settings → General → Device screen user readings, choose the formaldehyde or PM2.5 region, then another MQTT device as the source. Select the actual device and field. If missing, check that both versions support sensor descriptors and have published them.
3. Save the source, unit, display mode and lifetime, then select user readings. A configured double press can switch both regions together. The default lifetime is 20 seconds; expired or invalid data restores that region's original reading.
4. Close the browser and cloud platform. Publishing, subscribing and display continue while both devices and the broker run.

M1 has fixed three-digit segment displays and cannot show arbitrary unit text. The PM region is integer-only; the formaldehyde decimal point is fixed after its first digit. For W, V, A or ppm, hide the original legends and mg/m³ / µg/m³ units as appropriate. See [M1 display formats](m1-guide#v1-candidates-user-readings-and-pm25-smoothing).

## Protocol and troubleshooting

Sensor descriptors and readings use **MessagePack schema 3**, published through `metadata` and `state` respectively. Topics are `phicomm/v2/u_<account encoding>/sensor/<device_id>/metadata` and `/state`. Firmware derives the account encoding from the username; an empty username uses the local scope. Metadata is retained and readings are not. Boot identity, sequence, descriptor revision, validity and age prevent reuse of stale data. Diagnostic tools need MessagePack decoding support.

For an offline source, missing field, unit mismatch or expired sample, check source publications and M1 lifetime. Invalid or missing data is treated as unavailable. UDP pairing, web management authentication and broker authentication are independent. Developer interfaces are in the [openiot protocol documentation]({{PROTOCOLS_URL}}).
