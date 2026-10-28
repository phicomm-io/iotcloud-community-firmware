# Retained UDP runtime scenarios, native MessagePack coverage

Reference scenarios remain in `tests/test_runtime.c` and
`tests/passive_scenarios.inc`; this change does not delete or weaken them.
The native replacements compile production function bodies or include production
modules. They use simulated monotonic time and transport captures, not DUT load
or latency measurements. ASan/UBSan run on every executable.

| Retained scenario | Native replacement | Evidence boundary |
|---|---|---|
| `sensor_session_pacing`: initial MQTT grant + pending UDP read | `test_native_udp_runtime`, `pacing.c` | Actual `sensor_grant`/`sensor_poll`; MQTT MP envelope/token decoded by capture; UDP delivery boundary stubbed |
| Reconnect discards unconfirmed queued grant and retries | Same pacing fixture | Actual `feature_mqtt_session`; generation callback drops queue; next poll re-grants |
| 80 seconds healthy polling, including broker loss, emits no repeat grant | Same pacing fixture | Actual polling decisions; healthy reply state simulated exactly as retained scenario |
| Failed reads reauthorize at most once per 60 seconds | Same pacing fixture | Every intervening second checked; exact 60-second boundary checked |
| Healthy reconnect preserves installed token; account switch clears state | Same pacing fixture | Actual session handler and actual peer/sample structs |
| Group null envelope never emits | `test_group_mp`, `allocation_failures` | Actual `group_send_wire`/`group_mqtt_mp` null paths |
| Group root allocation failure, both LAN/MQTT modes | Same group fixture | Both wire-object/backing-buffer allocations failed, plus config snapshot allocation |
| Partial construction/buffer-growth failure never emits malformed payload | Same group fixture | Large real MP body forces buffer growth; allocation fails after root exists; zero transport emissions |
| Recovery probe root allocation failure | Same group fixture | Actual `group_poll`, each root allocation failed, no pending/no output |
| Pending-copy allocation failure | Same group fixture | Actual retry-slot allocation failure emits complete decoded MQTT fallback; no UDP/no pending |
| Async caller body modification/release cannot change pending command | `test_group_mp`, `source_lifetime_and_two_probe_recovery` | Actual `feature_group_control_mp`; source speed 60 changed to 99, overwritten and freed; pending and timeout fallback remain 60 |
| Group timeout isolates peer, increments counter, falls back to MQTT | Same group fixture | Actual `group_poll`; pending cleared, LAN disabled, counter +1, fallback payload checked |
| Two probes restore LAN, first alone cannot | Same group fixture | Actual probe writer/send/ACK parser; ACK delivered synchronously during send; first success leaves LAN false, second enables it |
| Read before authorization is ignored | `test_native_udp_runtime`, `passive.c` | Actual M1 `lan_sensor_input`, no reply/no binding |
| HTTP cannot issue temporary sensor grant | `test_service_mp` in native-tests owner | Actual ordinary command dispatcher returns unsupported; MQTT dispatcher alone handles grant |
| Retained MQTT grant ignored | `test_runtime_session_mp` in native-tests owner | Actual ingress route; authorization-business callback stubbed there; actual business tested here |
| Full uint32 boot, duplicate QoS1 grant, token validation | `test_native_udp_runtime` passive fixture and `test_sensor_grant_mp` | Actual grant function; UINT32_MAX preserved; duplicate grant accepted; zero/wrong-width tokens rejected |
| 32 alternating read/probe replies | Same passive fixture | Actual receiver/sample capture; actual MP encoder and decoder at captured sendto boundary |
| Wrong boot/token or unsupported sample operation cannot read/refresh | Same passive fixture | Actual receiver; reply count and last_seen unchanged |
| MQTT grant retry cannot make idle UDP peer ready | Same passive fixture | Actual grant function plus actual production status-loop writer; old last_read preserved |
| Successful UDP probe restores ready + zero idle | Same passive fixture | Actual receiver and status-loop writer |
| Idle session expiry; re-grant starts not ready | Same passive fixture | Exact expiry boundary; expired read ignored; fresh grant remains inactive |
| Four active sessions retained; fifth rejected | Same passive fixture and `test_sensor_grant_mp` | Actual bounded grant table; no eviction |
| Same MQTT account preserves RAM sessions; account change clears them | Same passive fixture | Actual `feature_mqtt_session` and actual session table |
| Grant/read/session activity does not write product configuration | Same passive fixture | Production functions link without product save/Flash callbacks; all state resides in RAM session table |
| Sensor metadata/snapshot units, freshness, sequence and borrowed lifetime | `test_sensor_mp` | Separate sensor owner production codec/consumer fixture |
| M1 unsupported binding/linkage/light/probe writes; time/sleep allowed | `test_native_features`, `test_m1_feature_mp.c` | Actual feature-domain functions; all original 405/200 assertions preserved |
| Binding output omits response_ids and advertises session_protocol 3 | Same feature fixture | Actual MP encode and borrowed decode; no response_ids, protocol=3 |
| M1 local sample does not autonomously publish MQTT | Same feature fixture | Actual local-sample copy + virtual_service + feature_mqtt_service, zero captured publications |

Run this lane without SDK/JSON dependencies:

```
python3 -m unittest tests.test_group_mp tests.test_native_udp_runtime tests.test_lan_client tests.test_sensor_grant_mp tests.test_lan_wire
```

The runner integration and the feature-owner follow-up live in the separately
owned native-tests lane. No production code, DUT, deployment or build artifact is
changed by this coverage patch.
