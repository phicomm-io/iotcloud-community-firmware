# Release Log

English translation of the published release history. Historical validation statements apply to the release named in each section, not to later firmware. Release notes are authored in the Wiki and frozen into the source repository before tagging.

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
