# Release Log

English translation of the published release history. Historical validation statements apply to the release named in each section, not to later firmware. Release notes are authored in the Wiki and frozen into the source repository before tagging.

## v0.9.20

- Fix A1 effects remaining lit after purifier power-off when custom brightness is used with fan-speed brightness tracking disabled. Keep automatic-mode zero-speed waiting distinct from power-off.
- Add colorful breathing on A1: the whole ring gradually changes color while breathing, with custom brightness and animation speed. Single-color breathing remains available.
- Fix TC1 Home Assistant states reverting to on after outlets are switched off over MQTT or IotCloud. Refresh all six outlets and the master switch after controls are saved, and republish discovery when device or outlet names change.
- Fix four-digit pressure readings overflowing or wrapping on M1 cards in IotCloud. The public MQTT service now grants Home Assistant startup-notification publishing to existing accounts as well, allowing integration reloads to trigger device rediscovery.
- Physical A1 and TC1 candidate upgrades preserved configuration. Acceptance covered light settings and power-off state, six-outlet MQTT states, renaming and rediscovery. The original TC1 outlet states and device name were restored afterward. M1 was not flashed for this release.
- Versions in the 0.9.x series are beta releases. Back up important settings before upgrading.

## v0.9.19

- Configure M1 sensor sources on A1 with an M1 MAC address and an optional LAN IP. M1 no longer needs pairing or an A1 allowlist; its page shows current sessions.
- Keep temporary session-token authorization over MQTT, then read sensors over the LAN. Remove sensor-link joint topics and the MQTT sensor-data fallback to reduce subscriptions, traffic and memory use. A1 group control remains available.
- A1 stops forwarding each M1's complete readings. Its own state includes only the aggregate values used for automatic control, and the page follows the same scope. Old proxy Home Assistant entities are removed on upgrade; use M1 directly for its full readings and entities.
- Add BMP388/BMP390 pressure readings to M1 MQTT reporting and Home Assistant discovery. Pressure uses hPa and becomes independently unavailable when the sensor disconnects or its readings expire.
- Upgrade both A1 and M1 to this version, preferably M1 first. MQTT is required to establish a new session or authorize again after M1 restarts. Existing valid sessions can keep reading over the LAN during a temporary broker outage; failed LAN reads follow the configured offline linkage behavior.
- Candidate firmware passed physical A1/M1 upgrades, LAN reads and a two-version read-only load comparison, alongside full/slim builds and host regression for all three models. Short load tests do not establish long-term stability; TC1 physical acceptance was not performed for this release.
- Versions in the 0.9.x series are beta releases. Back up important settings before upgrading.

## v0.9.18

- Fix intermittent A1 power-button failures after the device has been running: short presses show ring feedback and double-clicks switch automatic/manual mode again.
- Share A1 control handling across physical buttons, HTTP, MQTT and automation, reducing allocations and blocking in input handling while retaining child lock, custom button mappings, fan speed and ring behavior.
- Add optional direct Bemfa connectivity on TC1 and A1. The existing MQTT connection and Bemfa can be enabled independently or used together; existing connections and settings are preserved. M1 retains its existing connectivity.
- Further reduce status-query memory and embedded-page size. Build validation now checks A1 packages against the actual OTA staging capacity.
- Host regression and full/slim builds passed for A1, M1 and TC1. An A1 candidate was flashed and checked for preserved settings, Wi-Fi, MQTT and panel status; physical short-press and double-click tests passed. TC1, M1 and Bemfa physical acceptance was not performed for this release.
- Version 0.9.x is beta. Back up important settings before upgrading.

## v0.9.17

- Fix TC1 failing to connect to IotCloud, Home Assistant or a custom MQTT broker after initial setup or a factory reset despite a saved broker configuration.
- Applies to full and no-frontend-resources firmware. Preserve existing Wi-Fi, MQTT, schedules and outlet settings.
- The fix was flashed onto the TC1 that reproduced the problem; fresh status reports and cloud query responses were verified. No new physical A1 or M1 acceptance was performed for this release.

## v0.9.16

- Reduce A1, M1 and TC1 WebUI traffic: load complete status on first entry, poll live readings during normal use, and refresh network and diagnostics on demand or less frequently.
- Share the HTTP server, bounded connection management, request parsing and error handling across all three models.
- HTTP and MQTT reuse the authoritative device C structures, snapshot interface and JSON encoder. Copy state under short locks; release locks before encoding or sending.
- Unify MQTT connection, reconnect, command queues and sending. Prioritize command acknowledgements, bound memory under overload, and recover after load decreases while preserving protocol and retained-message compatibility.
- Fix TC1 abnormal-connection cleanup, OTA activation ordering and status synchronization. Reduce power/energy numeric conversion memory and handle allocation failures without unbounded growth.
- Retain command deduplication, conflict detection and acknowledgement targets while reducing receipt-cache memory. Preserve M1 rename acknowledgements before reconnecting.
- M1 supports optional BMP388/BMP390 sensors with automatic address/model detection and rediscovery after reconnection. Show hPa and the detected model in the existing overview and device summary; hide unavailable or expired readings.
- Enforce the actual four-device binding limit and improve narrow mobile layouts.
- Three devices completed at least four hours of read-only mixed HTTP/MQTT stress and recovery checks without observed unexpected whole-device restarts, deadlocks or sustained memory growth. Overload may return busy or time out; service recovers after load decreases. These results cover the tested devices and workloads, not every possible condition.
- Version 0.9.x is beta: later versions may change or remove features and interfaces. Keep backups of important settings.

## v0.9.15

- Fix TC1 freezes or reboots when opening the device page or reading live power and energy usage.
- Also improve reading and communication stability on A1 and M1, preserving existing Wi-Fi, MQTT, schedules, buttons and device settings.

## v0.9.14

- Reduce memory used by device linkage and MQTT communication. A1 application static RAM usage is reduced by about 10.8 KiB, and TC1 by about 1.0 KiB.
- A1 and M1 support up to four linked devices. After upgrading, existing configurations with more than four linked devices use only the first four.
- Improve web request queuing and MQTT command replies to reduce resource use during simultaneous operations. A busy device or one with insufficient memory can return `busy`, prompting you to retry later.
- Add memory headroom and command reply queue diagnostics to help investigate busy devices. Preserve existing Wi-Fi, MQTT, schedule, button and automatic-control settings.
- Compress the built-in A1, M1 and TC1 pages to reduce their firmware footprint while retaining existing features, Chinese and English languages, and controls.
- Offer full and slim firmware. Full firmware includes the complete device page, accessible at the device IP. Slim firmware, identified by `no-frontend-resources` in the filename, retains offline provisioning, initial MQTT setup and web password configuration; use the local device management page or IotCloud for everyday controls and complete settings. Online upgrades retain the current firmware variant.
- Both full and slim firmware support LAN management: enter the device LAN IP and web management password in the local device management page to control it directly, without an account, backend or MQTT service. Allow local network access in your browser.
- IotCloud lets you configure a device LAN address and prefers a direct LAN connection. It continues using MQTT when the direct connection is unavailable, and also offers an always-use-MQTT option.

## v0.9.13

- A1 breathing and rainbow marquee effects offer speed levels 1–10. Level 5 keeps the previous speed; the page preview follows the selected level and saved settings survive reboot.
- Scheduled tasks support shutdown countdowns from 1 minute to 24 hours, show remaining time and allow cancellation, deletion and restart in manual or automatic mode.
- Countdowns continue with the page closed and do not depend on network time. Editing or enabling a countdown restarts its full duration; reboot cancels active countdowns.
- The shared local/management A1 page exposes these controls. Chinese and English guides are updated, retaining existing CO₂ linkage and other settings.
- Host regression and Chromium/WebKit desktop/mobile tests passed. Physical A1 light, fan and upgrade acceptance was not performed for this release.

## v0.9.12

- A1 automatic fan control accepts linked M1 CO₂ alone or combined with PM2.5 and formaldehyde in fixed, linear or PID mode. Combined control uses the highest demand; hysteresis starts on any selected metric and idles after all targets are met.
- CO₂ freshness is independent. Unsupported M1 units do not contribute to its mean; missing selected CO₂ uses the configured offline speed. Expired UART readings do not block fresh CO₂.
- A1 publishes standard HA CO₂ discovery in ppm for selection in HomeKit Bridge. Existing explicit export lists must include the new CO₂ entity.
- SCD40 exports CO₂ only. Its auxiliary temperature/humidity are not displayed or forwarded to HA; temperature/humidity continue using the original M1 sensors. Upgrades remove auxiliary SCD40 discovery entries from earlier firmware.
- The shared local/IotCloud A1 page supports CO₂ means, combined metrics and ppm thresholds. Existing Wi-Fi, MQTT, bindings, schedules, effects, filter runtime and other settings are preserved.
- Includes existing main-branch A1/M1 provisioning-memory and web-response repairs, favicons, cache revalidation and bounded nonblocking HTTP writes, retaining the dad0514 first-flash initialization fix.

## v0.9.11

- M1 supports an optional external SCD40 CO₂ sensor over I²C. Connect SDA to GPIO8 and SCL to GPIO7, with signal pull-ups to 3.3 V; see the M1 guide for wiring.
- The local page automatically shows detected CO₂ in the existing metric area, aligns metric titles and quality badges, and displays CO₂ comfort ranges. An open page keeps expired readings in gray; reopening hides an expired CO₂ card.
- IotCloud device summaries show fresh CO₂ readings. Home Assistant discovers a CO₂ sensor in ppm, marks expired readings unavailable, and recovers when fresh data arrives.
- Retains the A1 first-flash configuration initialization fix for migration from other firmware (dad0514), plus existing automatic control, power-button behavior, and configuration migration.
- Preserves Wi-Fi, MQTT, bindings, and schedules. M1 devices without an SCD40 keep the original four metrics.

## v0.9.10

- Power-key presses while running no longer overlay green feedback on the mode/airflow display; green feedback is reserved for turning on from off.

- The default A1 power key starts the purifier when off and shows mode and actual airflow when on. Hold for 1–4 seconds, then release to power off. Double-click still toggles auto/manual mode while keeping the power state.
- Status feedback first confirms auto with two cyan pulses or manual with one warm amber pulse, then shows actual airflow with a speed bar in the same color. Auto idle shows zero airflow. The normal effect resumes afterward; child lock and night brightness limits apply.
- Migrate previous default power-key mappings while preserving custom gestures and other settings. Local and management pages can assign “Power on / show current mode and airflow”.
- Fix repeated automatic-control saves on the same page by updating the acknowledged configuration revision. Update Chinese and English guides and Wiki pages.

## v0.9.9

- A1 adds an independent Chinese “Operating mode” selector in Home Assistant. Automatic/manual selection preserves power and continues to synchronize fan presets and HomeKit Bridge.
- The management platform automatic-control card has an explicit settings button and clickable heading. Loading appears immediately, failed reads can be retried, and settings remain accessible while the purifier is off.
- Clarify the mode controls and refreshing HomeKit Bridge capabilities. Preserve Wi-Fi, MQTT, bindings, speed, automatic rules, filter, button and schedule settings.

## v0.9.8

- A1 automatic and manual modes are independent of power. Configure automatic speed limits, sensor-offline speed and independent night ranges. Air quality never turns on a purifier switched off by the user.
- Optional start/idle hysteresis runs at the start threshold and idles at the target, retaining state between them. PM2.5, formaldehyde and combined metrics support condition duration and minimum run/idle time. Fixed, linear or PID control adjusts speed while running.
- Local pages and the management platform support mode switching, automatic caps, targets and start thresholds. Platform device lists show automatic/manual mode and running, idle or off state.
- Double-clicking the power key switches modes by default. Cyan double pulses confirm automatic mode; one warm amber pulse confirms manual mode. Home Assistant modes synchronize with HomeKit Bridge while preserving power and remembered manual speed.
- Preserve Wi-Fi, MQTT, bindings, filter, button and schedule settings. Hysteresis defaults off and can be enabled after upgrading.

## v0.9.7

- A1 estimates filter life from fan running time. Adjust the rated lifetime and reset usage after replacing the filter; device pages, MQTT and Home Assistant show the same data.
- Configure A1 power-key, Wi-Fi-key and two-key combination gestures for effects, fan, light ring, child lock, filter reset and restart. Combination gestures suppress single-key actions and unintended network recovery.
- Improve A1 Chinese entity names, purifier icon and model in Home Assistant. Bound controls and physical filter entities share one device; filter commands still address the individual A1.
- Unify all three products on MiCO 3.6.4, preserve unassigned TC1 keys during legacy migration, and restore general settings navigation with improved touch interaction.
- Preserve Wi-Fi, MQTT, bindings, schedules and button settings while migrating filter data and the additional combination-key map.

## v0.9.6

- Devices can check versions, read release notes and download signed OTA directly from GitHub Pages HTTPS. The WebUI also retains both domestic and international HTTP mirror buttons.
- Downloads can be cancelled, and DNS, connection and I/O waits are bounded. Normal services resume after cancellation or failure. Final image commit cannot be cancelled; retries restart the download.
- OTA releases A1/M1 linkage workers and dynamic resources while MQTT/HTTP keep update status and cancellation available. Dictionary logs save 3584 bytes of RAM; TC1 HTTP stack use is reduced by about 12 KiB.
- Fix TC1 signed OTA boot-slot validation while retaining firmware signatures, model checks and Flash readback validation. HTTPS skips certificate validation; firmware signatures remain mandatory.
- Includes main-branch changes since v0.9.5: A1 panel, speed-ring and touch feedback fixes; A1/M1 stability and critical-worker watchdogs; independently pinned frontend releases, consent-based IotCloud connection and bilingual guides.
- A1, M1 and TC1 development candidates passed normal-load GitHub HTTPS signed updates, reboot, release notes, cancellation, bad-signature/tamper/truncation rejection and recovery. Sustained six-client pressure updating has not passed full acceptance: TC1 downloads can fail, and A1/M1 lack complete commit evidence. Reduce concurrent access during updates. Power-loss and complete resource-watermark tests remain outside this acceptance scope.

## v0.9.5

- Unified the three device WebUIs and Chinese / English interactions, improving status, averages, schedules and button mappings.
- Updated TMCP, bindings and A1 / M1 automation; the platform reuses device UIs and supports Legacy device names and meter readings.
- Improved MQTT parsing, acknowledgements and connection indicators; expanded host and browser regression coverage.
- Added build-configured China / international OTA sources, asynchronous checks and signed downloads.
- Added Cloudflare R2 / Worker publication and a China HTTP cache while retaining manual file and URL updates.
- Wiki release notes are frozen before tagging; publishing verifies the snapshot, checksums and mirror downloads.
- Added the project site, interactive device / platform demos, current downloads and historical archives.

## v0.9.4

Signed OTA for all three products, based on main commit `d50e937`. The implementation is unchanged from v0.9.3; the version was incremented and CI rebuilt / signed the artifacts.

- `.ota.bin.signed`: MQTT / URL and default web upload, with mandatory P-256 / SHA-256 verification.
- `.ota.bin.raw`: first transition from older firmware; the new WebUI accepts it after selecting **Allow unsigned firmware**, without a second dialog.
- `.all.bin`: native full Flash image.

Activation requires valid signatures, Flash readback digests and compatibility checks. Production private keys stay outside the repository and are used through a GitHub environment restricted to `v*` tags.

M1 web / MQTT rejection, installation and restart recovery were tested for v0.9.3. No device was flashed for v0.9.4. Physical TC1 / A1 validation, full peak-resource measurement and power-loss tests remained pending. Reinstallation and downgrade are allowed; URL downloads require direct HTTP 200 with Content-Length.

## v0.9.3

First signed OTA release for TC1 A1, M1 and A1. `.ota.bin.signed` is the P-256 / SHA-256 package for new-firmware default web upload and MQTT / URL installation. `.ota.bin.raw` is native OTA: use a trusted local web upload for the first transition from older firmware, or select **Allow unsigned firmware** in the new UI. `.all.bin` is the native full image; a bootloader must not receive the signed wrapper.

CI private keys remain outside the repository and are used only in the tag-restricted signing environment; the repository contains the public key. Signature checking happens before erasing. Flash readback SHA-256 and compatibility checks must pass before activation.

Physical M1 testing covered raw transition, raw / signed web uploads, rejection of bad signatures and raw MQTT packages, signed MQTT installation and restart recovery. Physical TC1 / A1, full resource peaks and power-loss / rollback remained pending. Reinstallation / downgrade are allowed; URL updates require direct HTTP 200 plus Content-Length.

Design / acceptance record: `localdocs/ota-signed-update-v1.md`; implementation PR #26. All three released products were downloaded and verified for P-256 signatures, signed digests and SHA256SUMS. M1 also installed the CI-signed release over MQTT; it restarted into `0.9.3-d50e93` and restored MQTT and sensor readings.

## v0.9.2

### Physically validated M1 baseline

This release records v0.9.2 as a usable physical-device baseline for original EMW3080B / MK3080B M1 hardware. Source: `c1c8f542eb1f61c34fbc12abca5aad681b7ff257` (PR #3); displayed version: `0.9.2-c1c8f5`.

### Changes

- The home page displays live air readings and screen brightness; secondary menus manage Wi-Fi, MQTT, device, binding, update and diagnostics. Unsaved inputs survive navigation.
- Corrected Wi-Fi / error icon channel mapping and low output when off. Normally Wi-Fi stays on and error stays off; both blink for Wi-Fi failure. For configured MQTT failure, Wi-Fi stays on while error emits two long / two short pulses. Unconfigured MQTT is not a fault.
- Fixed false MQTT idle disconnects and TCP-fragment timing across short yields; added error counts and last-error diagnostics.

### Physical M1 validation, 2026-10-14

- OTA through the device Web API restored Wi-Fi, MQTT and valid sensor readings after reboot.
- Temperature, humidity, formaldehyde and PM2.5 were read from real hardware.
- Platform testing passed account-topic discovery, five HA discovery configurations, browser brightness commands, setting acknowledgements and independent-client brightness restoration, without browser errors.
- An independent MQTT client observed 14 real sensor reports over 66 seconds without an offline event; automatic recovery after Broker restart was verified. This was not a long-duration stability test.
- The user confirmed normal short-press screen toggling and the final icon mapping: Wi-Fi on, error off.
- Host tests, WebUI tests and all three firmware CI builds passed.

Initial physical testing used functionally equivalent development firmware displayed as `0.9.1-da45b9-dirty`. Release binaries were built by the tag's CI; use that release's downloads and checksums.

### Known scope

Wi-Fi / MQTT fault timing passed host tests, but each physical fault-light pattern was not observed individually. Physical long-press reset, factory reset and network reset were not implemented in this release. The original M1 screen's formaldehyde / PM2.5 color thresholds and manual color controls had no verified protocol and were unsupported. Physical acceptance applied only to M1; A1 and TC1 passed builds, not physical acceptance.

### Download

For this historical M1 release, select `phicomm_m1.ota.bin` for web updates, not `.all.bin`, and verify SHA256SUMS. Wi-Fi, MQTT and brightness settings are retained.

## v0.9.1

### Changes

- Archived the TC1 SDK and unified firmware build entry points: [PR #1](https://github.com/FUjr/phicomm-community-firmware/pull/1), by @FUjr.
- Added account-scoped MQTT and Git tag firmware versions: [PR #2](https://github.com/FUjr/phicomm-community-firmware/pull/2), by @FUjr.

@FUjr made their first contribution in PR #1. [Full changelog](https://github.com/FUjr/phicomm-community-firmware/compare/0.0.0...v0.9.1).

## v0.9.0

[Full changelog](https://github.com/FUjr/phicomm-community-firmware/commits/v0.9.0).
