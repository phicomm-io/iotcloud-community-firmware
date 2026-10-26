# Release Log

English translation of the published release history. Historical validation statements apply to the release named in each section, not to later firmware. Release notes are authored in the Wiki and frozen into the source repository before tagging.

## v1.0.0-rc.1

The second v1.0.0 release candidate retains the features of the published v1.0.0-rc. This section lists the new DAIKIN hub and web-entry changes. Support for the hub’s own sensors in HomeKit is retained from the published RC.

### DAIKIN hub device list

- The hub automatically appears in its own device list, where its local sensors and device settings are accessible. The self entry does not consume any of the 12 external-device slots and cannot be removed or registered again as an external device.

### Local management and variant conversion

- Initial hub setup and local management no longer require a device web password to be configured first. An existing web password must still be authenticated, and the local hub account keeps its separate login password.
- A new full image advertising conversion support can switch through its LAN web page using a matching signed hub image and explicit confirmation, without first setting a web password. An existing password still requires authentication. Model, signature and installed OTA-slot checks remain in force, network settings are retained, ordinary updates remain variant-isolated, and direct hub-to-full OTA is unsupported.

### HomeKit status display

- After changing the HomeKit switch or resetting pairing, the page continues checking status while the hub restarts, shows the current state when the service returns, and prompts for sign-in if the session expires. It does not replay the change or reset request, or display stale pairing codes while waiting.

[Features since 0.9.27, device guides and downloads](https://phicomm-io.github.io/). Candidates and stable releases are shown separately. This section does not replace the complete notes for the published v1.0.0-rc.

## v1.0.0-rc

The v1.0.0 release candidate includes companion updates for device firmware, the M1 sensor MCU, WebUI and cloud platform.

### General and platform

- Device pages, the management platform, provisioning pages and service messages support Simplified Chinese / English. A separately saved device language controls MQTT / Home Assistant discovery names; changing it preserves entity identifiers and topics.
- Keep the existing interface style. “Local connection” uses a button that expands the device IP and password inputs when selected.
- New cloud usernames and MQTT aliases must contain 3–32 ASCII letters, digits, underscores, dots or hyphens. Existing long accounts can still sign in and add a short MQTT alias; email sign-in is handled separately.

### M1 sensors, buttons and display

- Show the Wi-Fi module and sensor MCU versions separately, or unknown when unavailable. **The open-source sensor MCU is recommended primarily for stock 11.x users; migration from 17.1.2.3 / 17.x is not recommended yet.** Wi-Fi OTA does not also update the sensor MCU.
- Add PM2.5 spike suppression and adaptive smoothing, following sustained increases quickly while retaining raw sensor-frame queries.
- Formaldehyde and PM2.5 threshold buzzer alarms can be enabled independently and are off by default. They are controlled separately from indicator colors, wake alarms and manual sound previews.
- With the matching new open-source sensor MCU, a single press toggles the screen. A double press only stops an alarm while it is ringing; otherwise, when configured, it switches both regions between original and user readings. Release after holding for at least 3 seconds but less than 10 to provision Wi-Fi again while retaining MQTT and other settings. Hold for 10 seconds to restore factory settings while retaining the controller’s independently stored music. Both MCUs restart after settings are saved; these actions also work offline.
- Configure independent sources, expiry times and colors for the formaldehyde and PM2.5 regions at the same time: local sensors, other sensors using the same MQTT account and broker, TC1 power, DAIKIN readings, A1 effective main-fan output percentage, and API / manual writes. A1 output is 0–100%, not RPM.
- The PM2.5 region displays a three-digit integer. The formaldehyde region supports integers, automatic scaling in thousands, two decimal places, one decimal place with a trailing zero, and fixed division by 1000. Show or hide the original labels and units independently. Each region’s nine lamp segments can be set individually or colored automatically using thresholds on the original physical value. The fixed glass units remain µg/m³ and mg/m³.
- Add the `display_profile.get/set` and `display_value.set` APIs. Direct writes specify a `0–999` number, decimal point, label and unit flags, and all nine lamp segments without source conversion or threshold coloring. The PM region rejects decimal points. Each region independently restores its original reading on expiry; immediate clearing and configuration-revision checks are supported. New configurations accept colon-separated or contiguous MAC addresses in either letter case.

### DAIKIN hub variant

- Add a separate `hub` variant with a local account, device management and hub settings alongside DAIKIN sensor functions, configurable directly through the WebUI.
- Provide an MQTT broker over TCP `1883` and WebSocket `9001/mqtt` for LAN devices and Home Assistant using a local account. The hub can also act as an upstream MQTT client, forward events in both directions as configured, and map and forward devices to Bemfa.
- Offer optional **HomeKit HAP bridging (experimental and off by default)** for TC1’s six switches, the A1 fan / light, and available valid sensors on M1 / DAIKIN in Apple Home. Matter is not included.
- A new `full` build that advertises conversion support can switch to the hub using the local web password, a signed `hub` image that fits the available capacity, and explicit confirmation, while retaining network settings. Normal OTA continues to distinguish variants; direct `hub → full` OTA is not supported.

### Configuration, updates and development

- Add full LAN configuration backup / restore protected by the web password, validating the model, variant and configuration version first. Retain supported migration paths for older configurations. Unsupported configurations enter a recovery mode that requires confirmation and preserves the original storage. M1 sensor MCU music and HomeKit pairing data are stored separately and excluded from ordinary Wi-Fi configuration backups.
- TC1 / A1 / M1 update pages can display and compute differences across versions directly. Unavailable information is explained and requires additional confirmation. M1 probes for new display capabilities before enabling them and retains compatible paths for the old APIs / UART.
- A1 / M1 EMW3080 release allocations use an 8 KiB reserve floor, while the test acceptance threshold remains 12 KiB. Allocations with 8–12 KiB of headroom are allowed.
- Organize CI, Issue / PR templates and contribution guidelines, covering interfaces, configuration migration and pinned dependencies across repositories. Archive v0.9.x; maintained code uses GPL-3.0, with third-party licenses retained.
- Keep user documentation organized by M1 / A1 / TC1 / DAIKIN for installation, updates, use and recovery, with the existing Pages style. Publish interfaces, protocols, configuration compatibility and examples together in the [developer documentation](https://phicomm-io.github.io/iotcloud-community-protocols/en/).

[Documentation and downloads by device](https://phicomm-io.github.io/en/) · [M1 sensor MCU update guide](https://phicomm-io.github.io/guides/en/m1-mcu-update.html)

## v0.9.27

- M1 Settings → General adds independent formaldehyde and PM2.5 two-level threshold editors and stock-default restoration. Existing persistence is retained; editing either pollutant preserves unsaved input for the other and handles shared revision conflicts. Defaults are 100/300 for formaldehyde and 75/150 for PM2.5 in controller raw units.
- M1 overview and scheduled alarms support four preset tones with preview. Availability checks MCU version ≥0.9.3, extension capabilities and PWM support; older controllers retain the original beep. One-time and weekly triggers, timeout and manual stop remain supported.
- M1 adds one custom RTTTL song: paste or import text, upload to the controller, read it back, play or stop, and select it for alarms. Limits are 1024 ASCII bytes, 256 notes and 180 seconds. Dual-slot MCU storage retains music across reboot and normal application OTA. A separate MCU v0.9.3 update and capability detection are required. Music remains quiet in some frequency ranges; no volume amplification is added.
- Companion MCU v0.9.3 is a separate development preview with nonblocking RTTTL playback, four short tones and a clock colon blinking once per second. Wi-Fi OTA does not include the controller image; controller network OTA replaces the application and preserves stock Boot.
- One M1 candidate passed Wi-Fi/controller network OTA, old-controller gating, real WebUI upload and exact readback, play/stop, music alarm timeout and song retention through another controller OTA. The user confirmed audible sound. Connection, CO₂, threshold and schedule settings were preserved. Threshold form save/restore passed host regression. Real interrupted power writes, sound pressure/oscilloscope measurements, long-term stability and other models were not tested.
- DAIKIN adds CO₂ module diagnostics and confirmed manual calibration against a known 400–1500 ppm environment stable for at least two minutes. Legacy CM1106 reserved status bytes are retained without false warmup flags. Timeouts remain unknown without retries; ACK does not verify accuracy.
- DAIKIN restores this unit’s stock climate model: subtract 4.9 °C, correct the humidity mixing ratio with q × 0.8915 + 0.2632, then convert using corrected temperature and bound to 0–100% RH. WebUI supports a custom temperature subtraction (default 4.9 °C), humidity adjustment (default 0 %RH), default restoration and persistent saving with readback confirmation.
- One candidate DAIKIN completed two OTAs, configuration retention, one confirmed 410 ppm calibration ACK and subsequent reading observation. Climate equations passed host reference and range tests. The formal signed image, independent climate accuracy, cold boot and long-term stability have not been accepted on hardware. M1 MCU v0.9.4 is unchanged; Wi-Fi OTA does not upgrade the controller.

## v0.9.26

- A1 adds the stock weighted filter-consumption model, estimated cumulative airflow, and switching between elapsed-time and weighted models. Existing configurations retain their model and accumulated runtime. Both counters continue running; model switching does not reset them, while filter reset clears both. Airflow is estimated from the speed mapping, not measured.
- M1 adds one-time and weekly wake alarms, web preview and stop controls, persistent stock formaldehyde/PM2.5 two-level threshold writes, negative temperatures, sensor health, MCU version queries and buzzer controls.
- With a capable MCU and detected CO₂ module, M1 can keep the stock display, replace it, or alternate at a configurable interval. Select the formaldehyde/PM number position and CO₂ thresholds. Requires open MCU v0.9.2; four-digit concentrations use a decimal in thousands, hiding the replaced pollutant label and unit.
- Add an authenticated M1 MCU network-upgrade bridge and companion tool. MCU v0.9.2 restores stock sensor/display cadence and PM reception/warmup branches, and adds threshold colors/alarms, 0x70 debug extensions and CO₂ rendering. The MCU remains a separate developer preview release.
- Physical A1 fresh-air and M1 candidates passed OTA, configuration preservation and Wi-Fi/MQTT recovery. Tests covered filter-model switching without counter reset, MCU network OTA/version readback, two CO₂ display-mode readbacks, preview/stop and actual one-time/weekly alarm triggers. Test settings were restored. Long-duration stability, cold power-on and oscilloscope waveforms were not tested.
- DAIKIN Settings → General → Indicator rules supports up to two thresholds for each of CO₂, TVOC and PM, with steady on, always off, fast blink and slow blink. Choose PM2.5 or PM10 as the PM lamp's data source. Formaldehyde rules appear when its module is detected. Rules survive restart; unmatched or stale readings turn the light off.
- The DAIKIN Wi-Fi lamp indicates system state: three long and three short pulses when unconfigured, fast blink when Wi-Fi is disconnected, slow blink while configured MQTT is not ready, and steady on when ready. It operates independently of the air quality indicator switch.
- DAIKIN recovery hotspots use `BRY88AX151K-AP-<last six MAC digits in uppercase>`, with IP and gateway `192.168.0.1`. Startup, button recovery and reopening after network failure share this configuration. Saved Wi-Fi and MQTT settings remain intact.
- New DAIKIN release assets use the `daikin_BRY88AX151K` prefix; normal upgrades use `daikin_BRY88AX151K.ota.bin.signed`. Historical archives retain their original filenames. Update packaging, signing, download catalogs and bilingual installation guides, distinguishing the older transition image included with the stock OTA tutorial.
- IotCloud remembers confirmed home-screen installation to prevent repeated Safari prompts. Standalone windows restrict zoom in the app and device frames. All website demos add a home link; update the DAIKIN settings demo and describe the project as open-source firmware for discontinued IoT devices or devices whose cloud services have ended.
- A DAIKIN candidate passed physical OTA, configuration preservation, Wi-Fi/MQTT recovery, hotspot naming and automatic shutdown, six sensor readings after warmup, and desktop/mobile page checks. The final signed release packages were not physically accepted byte for byte, and TC1 was not reflashed. Physical iPhone home-screen gestures and lamp blink measurements were not tested.

## v0.9.24

- Add universal ESP32 firmware for the DAIKIN BRY88AX151K, sharing the local WebUI, MQTT and sensor-session protocol. It detects PM2005 / PM2105 and the optional formaldehyde module; unavailable formaldehyde readings are hidden. DAIKIN has a full build only.
- The DAIKIN overview uses a single row of sensor cards, comfort scales and a separate PM10 card. It shows PM2.5, PM10, temperature, humidity, CO₂, TVOC and connected devices. TVOC includes a 300 ppb ventilation reference, with fixed-size indicator badges that fit longer labels.
- A1 automation offers metrics according to the provider's available data rather than its model. It accepts DAIKIN data and excludes missing or expired values from control. Existing binding and automation configurations remain compatible.
- A1, M1 and TC1 downloads use kernel TLS, handling short reads, short writes and nonblocking waits with handshake, cancellation and overall deadlines. Failure and retry release TLS sessions and sockets. HTTPS still does not validate certificates; OTA firmware signatures remain mandatory.
- Fix TC1 local button actions being lost when low memory prevents a JSON status query; local controls now execute directly. Separately fix restoration through the configuration API when an unconfigured MQTT host has the default port zero.
- The website Release Log adds reviewed Chinese/English history and comparisons across releases, scoped to model, firmware variant and the actual release chain. Missing evidence, translations or inconsistent archives produce explicit errors. Legacy OTA metadata, original single-release notes, download URLs and archived firmware files stay compatible.
- Full device pages add an upgrade comparison entry that passes the actual version and firmware variant to the website. Browsers calculate the notes; devices do not store release history or add note downloads. The website can optionally read LAN device information once, keeps manual selection after errors and never stores the password.

## v0.9.23

- Add optional A1 fresh-air variants using user UART TX for PWM and RX for fan feedback. Connect a compatible fan as described in the wiring guide. Standard variants retain their existing purpose and the original fan driver mapping is unchanged. All A1 PWM runs at 25 kHz.
- Control the fresh-air fan independently with manual or automatic speed, configurable PWM polarity and output mapping limits. Feedback is displayed in RPM with configurable pulses per revolution, defaulting to 2.
- Choose independent speed, a fresh-air limit proportional to the original fan, or a minimum original-fan speed driven by fresh-air demand. Master power-off stops both fans and existing coupling settings are preserved.
- Combine both automatic settings on one page with PM2.5, formaldehyde and CO₂ checkboxes. Improve speed transitions and constrained automatic control. Fresh-air controls support the web interface, MQTT and Home Assistant entities.
- Reduce temporary policy allocation and web resource size. Keep the 16 KiB memory protection threshold; busy responses remain possible under load.
- An A1 fresh-air candidate passed physical upgrade, configuration preservation and desktop/mobile page checks. Exact final release packages, HA/HomeKit pairing and electrical waveforms have not been physically validated. Off levels during cold boot and bootloader execution are not guaranteed. M1 and TC1 were not flashed for this release.
- Full fresh-air firmware has very little space remaining; a variant without embedded web resources is also available. Versions in the 0.9.x series are beta releases. Back up important settings before upgrading.

## v0.9.21

- Fix M1 restarting repeatedly during first migration from factory firmware by providing enough startup stack for factory configuration initialization.
- M1 factory migration must start with 0.9.21 or newer. Use the full M1 native OTA image for the first upload, rather than a full Flash image or a community signature wrapper. Devices already running community firmware continue to use signed OTA packages.
- A fix candidate passed factory LAN OTA, community hotspot web access, Wi-Fi setup, LAN access and sensor communication on one M1. The candidate was based on 0.9.19 with only the startup stack increased; this is not byte-for-byte physical acceptance of the final 0.9.21 packages. Power-cycle cold boot, A1, TC1 and public DNS migration were not tested.
- First migration may require joining the M1-AP hotspot and configuring Wi-Fi again. A closed upload connection alone does not establish success; verify the community version and device health as described in the migration guide.
- Versions in the 0.9.x series are beta releases. Back up important settings before upgrading.

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
