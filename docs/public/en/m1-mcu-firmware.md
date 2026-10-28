# M1 open-source controller firmware

The current GPL-3.0 source runs on the M1 sensor controller. Check which processor a firmware package targets before updating.

**Compatibility: open-source firmware is recommended for stock 11.x controllers. Stock 17.1.2.3 / 17.x differs substantially and awaits analysis; migration is currently discouraged.** Read the actual sensor-controller version. Do not infer it from the Wi-Fi version; unreadable versions remain unknown.

## Which MCU does this guide mean?

MCU means microcontroller. M1 contains two; their firmware and version numbers are separate:

| Name used in these guides | Chip and role | How to update |
| --- | --- | --- |
| Wi-Fi MCU / Wi-Fi module | EMW3080: Wi-Fi, local web interface and MQTT | Ordinary web firmware update; M1 Wi-Fi package |
| Sensor controller / controller MCU | ATSAMD20G18A: communicates with built-in sensors and drives the screen, button and buzzer | Computer script through the API; `m1-mcu-ota.bin` with its matching JSON |

**“Controller” on this page means the sensor controller.** Updating Wi-Fi to v1.0.0-rc.3 does not update this controller. To reduce the risk of beginners flashing the wrong image and making the device unusable, controller flashing has no WebUI upload button. Use the [API-based network update guide](m1-mcu-update); the script handles the API exchange and verification.

**Use stable Wi-Fi firmware v0.9.27 for everyday operation. RC releases are beta builds. Major changes are still expected, and daily use is not recommended.** To test v1 features, install **Wi-Fi v1.0.0-rc.3 and sensor controller v1.0.0-rc separately** for dual-region user readings, double-click actions and PM2.5 smoothing. Older features retain their minimum compatibility requirements: four tones and custom RTTTL music require controller v0.9.3 or newer; particle details and raw sensor frames require the capabilities published in controller v0.9.4. CO₂ on the physical screen requires controller v0.9.2 or newer and valid CO₂ data. Basic PM2.5, formaldehyde, temperature and humidity readings do not require this additional flash.

[Source](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20) · [Controller releases and downloads]({{SITE_URL}}#m1-mcu-releases) · [M1 operation](m1-guide)
[Network update guide](m1-mcu-update)

## Added capabilities

PM2.5, formaldehyde, temperature, humidity, clock and brightness remain the basic functions. Extensions need compatible Wi-Fi firmware that checks the controller version and reported capabilities.

| Capability | What the open-source controller adds | First version and compatibility requirements |
| --- | --- | --- |
| CO₂ on the physical screen | Replace the formaldehyde or PM field, or alternate at a configurable interval; expired data restores the original readings; CO₂ color thresholds | v0.9.2; requires an external sensor and valid readings from the Wi-Fi module; the original hardware cannot measure CO₂ on its own |
| Display and buzzer diagnostics | Override values, read/write the 29-byte segment RAM, test all segments and set PWM frequency/duration; temporary overrides expire | v0.9.2, new 0x70 extension protocol |
| Capability and state queries | Sensor health, display cache, PM mode and configured thresholds | v0.9.2, compatible tools or Wi-Fi firmware |
| Four notification tones | Tap, confirmation, notice and error tones with controller-side timeout | v0.9.3 development preview; Wi-Fi v0.9.27 |
| Custom RTTTL music | Upload, read, play and stop one monophonic tune; dual-slot Flash storage survives reboot and normal application OTA; compatible WebUI can use it for alarms | v0.9.3 development preview; Wi-Fi v0.9.27; 1024 bytes, 256 notes, 180 seconds maximum |
| Complete sensor frames | Two sets of PM1.0/PM2.5/PM10, six cumulative particle counts, raw formaldehyde, raw temperature/humidity and status; includes age, health and checksum information | Controller v0.9.4; Wi-Fi v0.9.27 provides WebUI readings; computer queries also supported |
| Auditable update and recovery | Open receive/stage/verification logic; the independent new Boot adds CRC32 journal checks and restarts interrupted copying | New Boot behavior requires replacing Boot; CRC is not a signature or old-version rollback |

Custom pollutant thresholds control indicator colors, with configuration readback. High-level buzzer alarms can be enabled separately for formaldehyde and PM2.5 and are off by default.

## Downloads and requirements

The download for testing v1 features is **controller v1.0.0-rc, a beta release candidate / prerelease**; stable v1.0.0 is not yet available. Use the image from that Release, reporting `1.0.0-rc-97a03b`. Obtain the pinned buffering-aware tools through the [network update guide](m1-mcu-update), rather than the older tools in the RC tag. Network migration from stock 11.x requires companion Wi-Fi v1.0.0-rc.3 and a check of its `mcu_ota_max_bytes` buffering capability.

Older compatibility requirements remain: controllers already running community firmware can use Wi-Fi v0.9.26 or newer for network updates and complete-frame queries. Web music, tone controls and particle details require at least Wi-Fi v0.9.27 and matching controller capabilities. These minimum versions do not replace the buffered-upgrade requirement for stock 11.x. The download section lists all public controller releases, including prereleases, separately from Wi-Fi firmware. Update the two MCUs separately.


- `m1-mcu-ota.bin` is the controller application linked at **0x4000**. Check its JSON manifest and SHA256SUMS.
- `m1-mcu-debug-recovery.zip` contains application/Boot debugging and recovery files with address instructions.
- Application OTA preserves the stock Boot, which does not gain the independent new Boot's CRC/recovery guarantees.
- Do not submit controller images to the EMW3080 Wi-Fi firmware updater. See [controller network OTA](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/blob/main/docs/network-ota.md).

## Read additional sensor data

With the test combination **Wi-Fi v1.0.0-rc.3 + sensor controller v1.0.0-rc** (older minimum: Wi-Fi v0.9.27 + controller v0.9.4 complete-frame capabilities), open the local M1 home page and find **Particle details**. It shows PM1.0, PM10 and six cumulative particle counts; PM2.5 remains in the main reading card. Expand the details for CF=1 concentrations and raw sensor diagnostics. “Two sets” means two concentration conventions (atmospheric and CF=1), not two physical particle sensors. Six counts mean particles larger than each size threshold per 0.1 L of air, not six separate concentrations.

No computer commands are needed for web readings. Missing support, warming up, unhealthy or expired data is not a zero reading; unavailable measurements show “—”. These readings also have MQTT reporting and Home Assistant discovery; see [M1 operation](m1-guide).

For computer queries, controller v0.9.4 and Wi-Fi v0.9.26 or newer are sufficient. Obtain the [matching source tools](m1-mcu-update), then enter your own IP and device ID:

<!-- os-picker -->

<!-- os:windows -->

```powershell
py -3 tools/m1_sensors.py --host DEVICE_IP --device-id DEVICE_ID
# Read only the complete particle frame
py -3 tools/m1_sensors.py --host DEVICE_IP --device-id DEVICE_ID --sensor pm
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 tools/m1_sensors.py --host DEVICE_IP --device-id DEVICE_ID
# Read only the complete particle frame
python3 tools/m1_sensors.py --host DEVICE_IP --device-id DEVICE_ID --sensor pm
```

<!-- /os -->

If a web password is configured, set `M1_WEB_PASSWORD` as described in the [network update guide](m1-mcu-update). The tool checks controller capabilities first and also supports offline frame decoding. PM1.0/PM2.5/PM10 use μg/m³, cumulative particle counts use particles per 0.1 L, and temperature/humidity conversions exclude M1 display compensation. Unknown bytes remain raw data.


## How the stock controller handles provisioning and Wi-Fi reset

The stock controller handles the body button and notifies Wi-Fi over UART. A short press switches the screen display; holding about 3 seconds starts provisioning; holding about 9 seconds and then releasing restores factory network and application settings. For ordinary provisioning, release after 3–4 seconds; see [stock Wi-Fi setup](m1-no-disassembly#1-provision-stock-wi-fi).

The controller also drives Wi-Fi **CHIP_EN** through PA24. An update commit restarts Wi-Fi as well, causing a temporary disconnection. This hardware restart alone does not clear Wi-Fi settings.

## Stock controller backup

The **ATSAMD20G18A stock 11.1.2.1 main Flash backup** is available for development and recovery on matching hardware. It contains 256 KiB (262144 bytes). It includes stock Boot and application, but **excludes NVM User Row, fuses and auxiliary calibration regions**. It is not Wi-Fi module firmware.

[Download stock controller backup](assets/firmware/m1-mcu/m1-atsamd20g18a-stock-11.1.2.1.bin) · [SHA256SUMS](assets/firmware/m1-mcu/SHA256SUMS)

```text
SHA-256 6a8a9f72741e01d5ebc73467819079146553ec73339029eaf4cc49e8cbd1a7f9
```

The full backup starts at **0x00000000**, and must not be used as application OTA payload. Save your own backup and verify chip/board, power and SWD recovery before restoring. Do not use this backup for a different controller version or board revision. The stock binary is provided separately as a factory backup and is not covered by the project's GPL-3.0 source license.

## v1.0.0-rc features

v1.0.0-rc includes double-click events, generic Wi-Fi display data and steadier PM2.5 readings. Matching Wi-Fi firmware can select CO₂ from another online device and manage units, thresholds and expiry. Expired data restores the original display; while an alarm rings, double-click first cancels it. This is a prerelease and requires matching controller, Wi-Fi firmware and frontend.

Formaldehyde and PM2.5 have separate over-threshold buzzer switches, both off by default. Light thresholds, alarms and manual tones remain separately controlled.
