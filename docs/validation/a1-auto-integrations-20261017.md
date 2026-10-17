# A1 automatic mode integration follow-up (2026-10-17)

## Scope and findings

- A1 `b0f893303134`, Home Assistant 2026.7.4, authenticated production management platform, and the bound real M1 were used. Other devices were not upgraded.
- v0.9.8 already advertised the fan `auto`/`manual` presets. HA's fan details exposed them as "Preset mode". The new normal Chinese `运行模式` select makes the independent setting easier to find; it dispatches `fan.mode.set` and reads physical A1 state.
- The original platform settings link opened during inspection. The card heading was plain text and waiting for MQTT settings had no immediate progress feedback. Both heading and explicit settings button now open the form, with loading/error/retry feedback; the entry is available while off or without a bound sensor.
- The reported Apple Home UI sequence was not directly reproduced. Existing HomeKit Bridge configuration was reloaded without changing pairing. No HomeKit power-command suppression was added: an explicit Active command still controls power.

## Candidate validation

Firmware candidate `acfdcff27e7431277725207b1db2cb6c679e43b3` was built, production-key signed, installed on the identified A1, and checked for ready panel, connected Wi-Fi/MQTT and zero hardware errors. Frontend candidate `0f8b482e041b6a9a8dde4bdd1a171d28d576f60a` was exercised with the actual authenticated account and MQTT connection.

| Check | Result |
| --- | --- |
| Firmware host scenarios, production host builds and full MQTT suite | PASS |
| HA discovery select options, rendered commands/state and power independence | PASS |
| Frontend unit tests, A1 automatic-control forms, syntax and mobile/desktop layout | PASS |
| Real HA select UI: manual/auto while off, physical state readback | PASS |
| Native installed HomeKit air-purifier callbacks, isolated HA using production MQTT and physical A1 | PASS |
| HomeKit auto target while off: reports automatic target and Active=0; A1 stays off | PASS |
| HomeKit manual target while running: current displayed percentage and manual mode converge | PASS |
| Platform settings entry/loading, threshold save and physical readback | PASS |
| Paired iPhone Apple Home UI reproduction | NOT_RUN |

Platform settings were saved with temporary PM2.5 start/target values 61/29 and confirmed at the device, then restored to the original 50/35. Manual speed was restored to 46. Configuration backups and detailed browser/MQTT/runtime artifacts are kept outside the repository; credentials and private configuration are excluded.

Transient HTTP `503 busy` responses after configuration writes were observed; read checks waited for the device to become available. Such responses are not recorded as successful reads. Final release installation and public deployment are separate from these candidate results.
