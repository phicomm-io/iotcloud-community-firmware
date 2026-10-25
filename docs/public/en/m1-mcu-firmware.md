# M1 open-source controller firmware

M1 has two independently updated processors. The **EMW3080 Wi-Fi module** runs networking, the web interface and cloud communication. The **ATSAMD20G18A controller** drives sensors, the display, RTC, button and buzzer. This MIT-licensed project implements the controller firmware. Installing Wi-Fi firmware does not automatically replace it.

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
| Complete sensor frames | Two sets of PM1.0/PM2.5/PM10, six cumulative particle counts, raw formaldehyde, raw temperature/humidity and status; includes age, health and checksum information | v0.9.4; computer tool over the Wi-Fi bridge |
| Auditable update and recovery | Open receive/stage/verification logic; the independent new Boot adds CRC32 journal checks and restarts interrupted copying | New Boot behavior requires replacing Boot; CRC is not a signature or old-version rollback |

Fixes the stock firmware issue where two-level pollutant threshold settings did not take effect. Custom thresholds now control indicator colors and high-level buzzer alarms, with an added query for the current configuration. v0.9.3 also blinks the clock colon each second, but the corresponding stock path remains unconfirmed, so this is not claimed as a stock-exclusive difference.

## Downloads and validation

The latest published controller release is **v0.9.4, a development preview**. Network flashing and sensor-frame queries require Wi-Fi v0.9.26 or newer; web music and tone controls require Wi-Fi v0.9.27 or newer. The dedicated download section lists public releases including prereleases, separately from Wi-Fi firmware. Update the controller and Wi-Fi module separately.

**This remains a development preview.** The watchdog clock divider causing the early oss-0.3 boot failure has been fixed. This candidate passed network OTA, preset tones, WebUI music upload/readback/play/stop and song retention across application OTA on one M1; the user confirmed audible sound. Candidate validation is recorded separately from acceptance of the official Release binary. Other board revisions, full electrical timing and real interrupted-write recovery remain unverified. Quiet music remains a known limitation; this release does not add volume amplification. Check the downloaded tag's release notes and [validation record](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/blob/main/docs/validation.md).

- `m1-mcu-ota.bin` is the controller application linked at **0x4000**. Check its JSON manifest and SHA256SUMS.
- `m1-mcu-debug-recovery.zip` contains application/Boot debugging and recovery files with address instructions.
- Application OTA preserves the stock Boot, which does not gain the independent new Boot's CRC/recovery guarantees.
- Do not submit controller images to the EMW3080 Wi-Fi firmware updater. See [controller network OTA](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/blob/main/docs/network-ota.md).

## Read additional sensor data

v0.9.4 provides read-only complete-frame queries. Use the tool from this version's source with your own device IP and ID:

```sh
python3 tools/m1_sensors.py --host DEVICE_IP --device-id DEVICE_ID
# Read only the complete particle frame
python3 tools/m1_sensors.py --host DEVICE_IP --device-id DEVICE_ID --sensor pm
```

If a web password is configured, set `M1_WEB_PASSWORD` as described in the [network update guide](m1-mcu-update). The tool checks controller capabilities first and also supports offline frame decoding. PM1.0/PM2.5/PM10 use μg/m³, cumulative particle counts use particles per 0.1 L, and temperature/humidity conversions exclude M1 display compensation. Unknown bytes remain raw data.

A development candidate on one M1 returned PM1.0/PM2.5/PM10 of 13/19/19 μg/m³ and nonzero particle counts, with valid measurement, status and auxiliary checksums. Additional data currently requires on-demand computer queries; automatic Wi-Fi collection, web display and extra MQTT reporting are not included. Ordinary PM2.5 reporting is unchanged. Module identity and measurement accuracy need independent verification. See [fields and hardware observations](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/blob/v0.9.4/docs/sensor-frames.md).

## Stock controller backup

The analyzed **ATSAMD20G18A stock 11.1.2.1 main Flash backup** is available for development/recovery after checking the hardware and backup scope. It contains 256 KiB (262144 bytes); two original reads matched byte for byte. It includes stock Boot and application, but **excludes NVM User Row, fuses and auxiliary calibration regions**. It is not Wi-Fi module firmware.

[Download stock controller backup](assets/firmware/m1-mcu/m1-atsamd20g18a-stock-11.1.2.1.bin) · [SHA256SUMS](assets/firmware/m1-mcu/SHA256SUMS)

```text
SHA-256 6a8a9f72741e01d5ebc73467819079146553ec73339029eaf4cc49e8cbd1a7f9
```

The full backup starts at **0x00000000**, and must not be used as application OTA payload. Save your own backup and verify chip/board, power and SWD recovery before restoring. Other board revisions have not been accepted on this evidence. The stock binary is provided separately as a factory backup and is not covered by the project's MIT source license.
