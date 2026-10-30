**RC releases are Beta builds. Major changes are still expected; everyday use is not recommended. The stable channel remains on v0.9.27.** The current candidate is **v1.0.0-rc.4**. This page describes current features; earlier candidates retain their original notes and validation limits. Wi-Fi firmware, the M1 sensor controller, frontend and cloud platform are released separately. Updating one does not update the others.

[Candidate downloads and release notes](release-log.html?channel=candidate) · [Choose an update image](firmware-update.html)

#### RC4: communication, web interface and S30-V1B

- **Binary business interfaces.** HTTP, device MQTT and UDP default to MessagePack. Legacy JSON business paths are removed; compact JSON remains for Home Assistant. Use the matching frontend and a cloud platform supporting device discovery. Existing JSON scripts require changes.
- **Separate business state and diagnostics.** Coalesce changes before encoding, reuse unchanged snapshots, and fetch shared numeric diagnostic fields on demand with `diagnostics.get`. MQTT and HA track delivery independently; command receipts are separate.
- **Fewer duplicate web requests.** Serialize and coalesce HTTP reads, prioritize writes and bound confirmation reads. Cancel stale reads and isolate old responses. Automation settings retain previous content and unsaved edits. A1 renders animation locally; ordinary A1/M1 state reads run about every five seconds, with faster confirmation after controls. This does not establish new P95 results.
- **Authentication and LAN automation.** Unset device passwords permit direct access without consuming login sessions. Configured passwords use tokens with admin Basic retained. mDNS discovery and password-paired UDP coexist with existing MQTT sensor-session grants. All three MXCHIP products allow at most two waiting requests and return bounded overload responses.
- **Migration and builds.** Recognized 0.9.x layouts can migrate; unknown layouts enter protected recovery. Frontend resources follow device capabilities. MXCHIP uses the GCC14 compiler with GCC5 runtime libraries; S30 uses GCC5. Size, build checks and physical stability are validated separately.
- **S30-V1B outlet.** ORVIBO S30-V1B uses BL2028N/BK7231N/HLW8032 and provides outlet control, voltage, current, power, energy and persistent multiplier settings. Native RBL and bootloader-preserving STOCK_UA files use the stock layout by default. Initial network installation from stock `2M.1220` and community URL OTA were tested on the unit; first installation requires Wi-Fi setup again. Return to stock and dual boot are not provided. Other stock versions, reference-meter accuracy and power-loss recovery remain unverified.

[S30-V1B network and serial installation](s30-flash.html) · [Operation, LEDs and recovery](s30-guide.html)

#### All devices and the platform

- **Chinese and English interfaces and device language.** Device controls, setup pages, platform screens, service responses and email support Simplified Chinese / English while retaining the existing design. A separately saved device language controls MQTT / Home Assistant names. Browser language is independent; entity identifiers, unique IDs and topics remain unchanged. Compact images retain their setup pages; the separate frontend provides full controls.
- **Local connection opens on demand.** The standalone device manager, platform and Pages demos use a Local connection button that reveals collapsible IP and password inputs.
- **Complete configuration backup and restore.** Download or restore settings over LAN HTTP protected by the device web password, validating model, variant, configuration version and contents first. Supported older configurations migrate. Unsupported layouts retain storage until clearing is confirmed. Sensor MCU music and HomeKit pairing are excluded from ordinary Wi-Fi backups.
- **Review differences before upgrading.** TC1 / A1 / M1 update pages fetch and verify structured release data in the browser and calculate changes from the current version to the target. Unavailable data is explained and requires additional confirmation before continuing.
- **New platform names fit MQTT topics.** New usernames and MQTT aliases contain 3–32 ASCII letters, digits, underscores, dots or hyphens. Existing longer accounts retain login and may add a shorter MQTT alias. Email login is separate. This does not change account rules for third-party brokers. Platform services deploy separately from device OTA.

#### M1: sensors, buttons and both display regions

- **Separate firmware versions.** The page shows the original Wi-Fi and sensor MCU versions separately, or unknown when unreadable. The open-source controller is recommended first for stock **11.x; migration from 17.1.2.3 / 17.x is currently discouraged**. Wi-Fi OTA does not update the sensor MCU.
- **Steadier PM2.5 and optional buzzer alarms.** The matching open-source controller uses a three-sample median and adaptive smoothing to suppress spikes while following sustained increases quickly. Raw sensor frames remain available. Formaldehyde / PM2.5 threshold buzzer switches are separate and off by default, independent of lamps, scheduled alarms and manual tones. Web controls verify saved settings and show capability limits on older controllers.
- **Single press, double press and separate holds.** With the matching controller, a single press toggles the screen. A double press first stops a ringing alarm; otherwise it can switch both regions between original and user readings. Release after holding at least 3 but under 10 seconds to clear Wi-Fi and the web password while retaining MQTT and other settings. Hold for 10 seconds to reset network-module settings, retaining independent controller music. Both MCUs restart after settings save successfully, including offline.
- **Independent data for each region.** Configure local sensors, paired LAN devices or authorized MQTT sensors, or API / manual values for both formaldehyde and PM2.5 regions. Sources include external CO₂ and other sensors, DAIKIN readings, TC1 power and effective A1 main-fan output. Each region has its own expiry. Missing, expired, offline or invalid data restores only that region without changing physical acquisition or alarms.
- **Formats and legends match the glass.** PM2.5 supports three integer digits up to 999. The formaldehyde region supports integer, automatic kilo scaling, two decimals, one decimal with a trailing zero, and fixed division by 1000. Its decimal point is fixed after the first digit. Native legends and units can be hidden separately; the glass still has only µg/m³ and mg/m³, with no new ppm, W or % symbols. Positive local / MQTT overflow is limited to the selected mode maximum.
- **Nine lamp segments per region.** Red, green and yellow segments at each of three positions can be controlled independently, including multiple colors at one position. Alternatively, apply two thresholds to the original physical value before display conversion.
- **Web configuration and raw API.** The full page configures sources, expiry, format, legend / unit visibility, lamps and double-press action for both regions, with a fixed-glass preview and MCU support / confirmation status. `display_profile.get/set` uses revision checks and accepts compact or colon-separated MACs in either case. Probe capabilities first; raw API / UART and defined configuration migration remain available; the former MQTT feed and HTTP display-profile shapes are replaced in RC.1. For regions configured as API inputs, `display_value.set` writes integer digits 0–999, the decimal point, legends and nine lamps directly, without scaling, rounding, clamping or threshold colors. Fractional / out-of-range digits and PM decimal points are rejected; TTL 0 clears only that region.
- **Buffered controller image forwarding.** Supporting Wi-Fi firmware receives the complete image uploaded by the computer, then forwards it to the sensor MCU over UART. The existing per-frame HTTP bridge remains available. The controller updates independently; first check device capabilities and image requirements. Application OTA retains the stock Boot. See the [controller update guide](m1-mcu-update.html) for the stock 11.x tool, capacity and runtime requirements.

[M1 operation](m1-guide.html) · [Controller features and version requirements](m1-mcu-firmware.html)

#### TC1: provide power to other devices

TC1 publishes power for M1 user readings with device identity, freshness and validity markers. Pair the LAN devices or use an existing MQTT sensor-session grant. Unavailable readings are not represented as zero. Existing outlet controls, schedules and metering remain available and are not counted again as new features.

[TC1 operation and connection](tc1-guide.html)

#### A1: provide effective fan output

A1 publishes the main fan's effective **0–100%** output, including the fresh-air variant, for M1 displays. This is neither RPM nor a requested level that has not taken effect. Data includes identity, freshness and validity; unavailable values are not represented as zero. EMW3080 release images for A1 / M1 allow allocation down to an 8 KiB reserve while retaining a 12 KiB test-acceptance threshold.

[A1 operation and connection](a1-guide.html)

#### DAIKIN: a separate hub edition

- **Independent hub image and console.** Adds device discovery, pairing, management and hub settings alongside sensor functions. The embedded `/iotcloud/#/` console configures upstream MQTT, Bemfa and HomeKit, without cloud email registration or GitHub login.
- **mDNS and UDP replace the embedded Broker.** RC4 no longer hosts MQTT TCP 1883 or a WebSocket Broker. The hub discovers devices through mDNS, uses paired UDP communication and forwards upstream MQTT and Bemfa messages. Local Home Assistant and MQTT clients must use an external Broker; change old embedded-Broker connections.
- **Optional HomeKit HAP bridge.** Off by default, this experimental feature provides TC1's six Switches, A1 fan / light, and valid M1 / DAIKIN sensors. The hub’s own sensors are included automatically. **Matter is not included.**
- **Explicit conversion to hub.** Supporting `full` firmware accepts a signed `hub` image that fits the installed slot, with explicit confirmation and retained network settings. The current rc.4 retains the optional device Web password introduced in rc.1; the original rc password requirement is historical. Ordinary OTA preserves variants; direct `hub → full` OTA is unsupported. Conversion does not enlarge OTA partitions: check the actual slot capacity.

[DAIKIN operation, installation and hub settings](daikin-guide.html)

#### Documentation and development

Installation, updates, use and recovery guides remain grouped by M1 / A1 / TC1 / DAIKIN / S30-V1B. Bilingual [developer documentation](https://phicomm-io.github.io/iotcloud-community-protocols/en/) centralizes interfaces, protocols, configuration compatibility and examples. The v0.9.x artifacts and notes remain archived. CI, issue / PR templates, contribution rules and pinned cross-repository dependencies have been organized. Maintained code uses GPL-3.0; third-party notices remain applicable.

#### Wi-Fi firmware v1.0.0-rc.1: generic fields and hub updates

RC.1 adds the following changes to the original RC:

- Devices publish sensor-data schema 2 with stable field IDs and `name/type/unit/value/valid/age_ms` descriptors. Current values use `number`; invalid readings use `null`, never a synthetic zero. TC1 power, effective A1 fan output and M1 / DAIKIN sensors follow the same contract.
- M1 display-profile schema 4 binds a source by device + field ID + expected type/unit, without model adapters. Matching numeric fields can be added by source devices without a new model mapping. A new complete snapshot immediately revokes missing, invalid, mismatched or unsupported fields; an empty snapshot revokes all previous candidates. Saved bindings remain. Future field types are reserved and are not converted to numbers. Custom clients must migrate from RC interfaces and use the matching frontend.
- The device Web password becomes optional for initial hub-owner setup and explicit full → hub conversion. Existing passwords still require Basic `admin` authentication; separate hub-account password requirements remain unchanged.
- The management list automatically starts with a read-only local device, in addition to twelve user-registration slots. Legacy manual local entries are deduplicated in responses without persistent writes. Manual duplicate additions and deletion of the local device are rejected.
- After restart, the console recovers through read-only requests. HTTP 401 from an expired session returns to login without replaying configuration writes or reboot requests.

Use the [release notes](release-log.html?channel=candidate) for actual candidate availability and confirmed changes. IoTCloud platform versions are independent of Wi-Fi firmware versions.

#### Retained rc.2 fixes and new rc.3 recovery sequences

- **The rc.2 configuration and web recovery fixes remain.** With incompatible configuration, M1 no longer reports a network-only reset as a successful full clear. Full and minimal pages refresh recovery state, wait for restart after an explicitly confirmed clear and allow Wi-Fi setup to continue. The full Reboot and recovery page displays its controls. This adds no automatic migration for unknown third-party layouts.
- **rc.3 adds A1 alternative recovery sequences.** For panels that report release after about 2–3 seconds of continuous contact, tap Wi-Fi twice, hold the third touch for 1–2 seconds and release to reset networking; tap three times, hold the fourth touch for 1–2 seconds and release to factory reset. Release each short tap, leave less than 0.3 seconds between touches and hold the final touch for at least 1 second and less than 5 seconds. Recovery runs on release and bypasses child lock. Existing 5 / 10 second recovery holds remain. The new sequences await physical acceptance; see [A1 recovery](a1-guide.html#recovery-gestures).
