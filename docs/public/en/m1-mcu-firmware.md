# M1 open-source controller firmware

The current GPL-3.0 source runs on the M1 sensor controller. Check which processor a firmware package targets before updating.

**Compatibility: open-source firmware is recommended for stock 11.x controllers. Stock 17.1.2.3 / 17.x differs substantially and awaits analysis; migration is currently discouraged.** Read the actual sensor-controller version. Do not infer it from the Wi-Fi version; unreadable versions remain unknown.

## Which MCU does this guide mean?

MCU means microcontroller. M1 contains two; their firmware and version numbers are separate:

| Name used in these guides | Chip and role | How to update |
| --- | --- | --- |
| Wi-Fi MCU / Wi-Fi module | EMW3080: Wi-Fi, local web interface and MQTT | Ordinary web firmware update; M1 Wi-Fi package |
| Sensor controller / controller MCU | ATSAMD20G18A: communicates with built-in sensors and drives the screen, button and buzzer | Computer script through the API; `m1-mcu-ota.bin` with its matching JSON |

**“Controller” on this page means the sensor controller.** Updating Wi-Fi to v0.9.27 does not update this controller. To reduce the risk of beginners flashing the wrong image and making the device unusable, controller flashing has no WebUI upload button. Use the [API-based network update guide](m1-mcu-update); the script handles the API exchange and verification.

Wi-Fi v0.9.27 adds web controls that need a separately updated open-source sensor controller: four tones and custom RTTTL music require controller v0.9.3 or newer; particle details and raw sensor frames require the capabilities published in controller v0.9.4. CO₂ on the physical screen requires controller v0.9.2 or newer and valid CO₂ data. Basic PM2.5, formaldehyde, temperature and humidity readings do not require this additional flash.

[Source](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20) · [Controller releases and downloads]({{SITE_URL}}#m1-mcu-releases) · [M1 operation](m1-guide)
[Network update guide](m1-mcu-update)

## Added capabilities

PM2.5, formaldehyde, temperature, humidity, clock and brightness remain the basic functions. Extensions need compatible Wi-Fi firmware that checks the controller version and reported capabilities.

| Capability | What the open-source controller adds | Availability |
| --- | --- | --- |
| CO₂ on the physical screen | Replace the formaldehyde or PM field, or alternate at a configurable interval; expired data restores the original readings; CO₂ color thresholds | v0.9.2; requires an external sensor and valid readings from the Wi-Fi module; the original hardware cannot measure CO₂ on its own |
| Display and buzzer diagnostics | Override values, read/write the 29-byte segment RAM, test all segments and set PWM frequency/duration; temporary overrides expire | v0.9.2, new 0x70 extension protocol |
| Capability and state queries | Sensor health, display cache, PM mode and configured thresholds | v0.9.2, compatible tools or Wi-Fi firmware |
| Four notification tones | Tap, confirmation, notice and error tones with controller-side timeout | v0.9.3 development preview; Wi-Fi v0.9.27 |
| Custom RTTTL music | Upload, read, play and stop one monophonic tune; dual-slot Flash storage survives reboot and normal application OTA; compatible WebUI can use it for alarms | v0.9.3 development preview; Wi-Fi v0.9.27; 1024 bytes, 256 notes, 180 seconds maximum |
| Complete sensor frames | Two sets of PM1.0/PM2.5/PM10, six cumulative particle counts, raw formaldehyde, raw temperature/humidity and status; includes age, health and checksum information | Controller v0.9.4; Wi-Fi v0.9.27 provides WebUI readings; computer queries also supported |
| Auditable update and recovery | Open receive/stage/verification logic; the independent new Boot adds CRC32 journal checks and restarts interrupted copying | New Boot behavior requires replacing Boot; CRC is not a signature or old-version rollback |

Custom pollutant thresholds control indicator colors and high-level buzzer alarms, with a query for the current configuration.

## Downloads and requirements

The latest published controller release is **v0.9.4, a development preview**. Network flashing and sensor-frame queries require Wi-Fi v0.9.26 or newer; web music, tone controls and particle details require Wi-Fi v0.9.27 or newer and matching controller capabilities. The dedicated download section lists public releases including prereleases, separately from Wi-Fi firmware. Update the controller and Wi-Fi module separately.


- `m1-mcu-ota.bin` is the controller application linked at **0x4000**. Check its JSON manifest and SHA256SUMS.
- `m1-mcu-debug-recovery.zip` contains application/Boot debugging and recovery files with address instructions.
- Application OTA preserves the stock Boot, which does not gain the independent new Boot's CRC/recovery guarantees.
- Do not submit controller images to the EMW3080 Wi-Fi firmware updater. See [controller network OTA](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/blob/main/docs/network-ota.md).

## Read additional sensor data

With **Wi-Fi v0.9.27 or newer + sensor controller v0.9.4 capabilities**, open the local M1 home page and find **Particle details**. It shows PM1.0, PM10 and six cumulative particle counts; PM2.5 remains in the main reading card. Expand the details for CF=1 concentrations and raw sensor diagnostics. “Two sets” means two concentration conventions (atmospheric and CF=1), not two physical particle sensors. Six counts mean particles larger than each size threshold per 0.1 L of air, not six separate concentrations.

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


## Stock controller backup

The **ATSAMD20G18A stock 11.1.2.1 main Flash backup** is available for development and recovery on matching hardware. It contains 256 KiB (262144 bytes). It includes stock Boot and application, but **excludes NVM User Row, fuses and auxiliary calibration regions**. It is not Wi-Fi module firmware.

[Download stock controller backup](assets/firmware/m1-mcu/m1-atsamd20g18a-stock-11.1.2.1.bin) · [SHA256SUMS](assets/firmware/m1-mcu/SHA256SUMS)

```text
SHA-256 6a8a9f72741e01d5ebc73467819079146553ec73339029eaf4cc49e8cbd1a7f9
```

The full backup starts at **0x00000000**, and must not be used as application OTA payload. Save your own backup and verify chip/board, power and SWD recovery before restoring. Do not use this backup for a different controller version or board revision. The stock binary is provided separately as a factory backup and is not covered by the project's GPL-3.0 source license.

## v1.0 in development

Plans include double-click events, generic Wi-Fi display data and steadier PM2.5 readings. Matching Wi-Fi firmware can select CO₂ from another online device and manage units, thresholds and expiry. Expired data restores the original display; while an alarm rings, double-click first cancels it. These features have not been released and require matching controller, Wi-Fi firmware and frontend.

v1 also plans separate over-threshold buzzer switches for formaldehyde and PM2.5, both off by default. Light thresholds, alarms and manual tones remain separately controlled.
