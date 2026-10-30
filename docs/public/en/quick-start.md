# Quick start

This project provides open-source firmware for discontinued IoT devices or devices whose cloud service providers have ended support. It supports the Phicomm TC1 power strip, M1 air quality monitor, A1 air purifier, DAIKIN BRY88AX151K air quality monitor and ORVIBO S30-V1B smart socket. It provides local web control, MQTT and Home Assistant integration. Firmware downloads are free; the manufacturer's cloud is not required.

**v0.9.27 is the stable release. v1.0.0-rc.4 is for beta testing and adds S30-V1B support.** See the [complete cumulative candidate notes](release-log.html?channel=candidate). Downloads also retain the 0.9.x archive. M1 Wi-Fi and sensor-controller firmware update separately; the open-source controller is recommended for stock 11.x. Migration from 17.1.2.3 / 17.x is currently discouraged.

## Identify your device

| Device | Functions | Hardware |
| --- | --- | --- |
| TC1 power strip | Six outlets, metering, child lock, indicators and schedules | A1 hardware revision, EMW3031. **A2 hardware is not supported.** |
| M1 air quality monitor | PM2.5, formaldehyde, temperature, humidity and screen brightness | Original EMW3080B / MK3080B module; retain the ATSAMD20 |
| DAIKIN BRY88AX151K | PM2.5, PM10, CO₂, temperature, humidity, TVOC and optional formaldehyde; A1 automation | ESP32 / 4 MB Flash; PM2005 / PM2105 auto detection |
| A1 air purifier | Fan, light ring, child lock, night mode and environmental-data automation | Original EMW3080B / MK3080B module |
| ORVIBO S30-V1B | Relay, voltage, current, power, energy and power-on/off policy | BL2028N / BK7231N, HLW8032 meter |

**`tc1(a1)` means the A1 hardware revision of the TC1 power strip, not the A1 air purifier. They are different products and cannot use each other's firmware.** Check the model before flashing.

| Device | First installation | Wired installation and recovery |
| --- | --- | --- |
| TC1 A1 hardware | [Illustrated OTA guide](tc1-no-disassembly-ethernet-wifi-ap), stock 1.0.0.24 / 1.0.0.26 | [J-Link](tc1-flash) |
| M1 | [Factory Wi-Fi setup and OTA installation](m1-no-disassembly), starting at 0.9.21 | [Serial (recommended) / J-Link](m1-flash) |
| A1 air purifier | [Factory Wi-Fi setup and OTA installation](a1-no-disassembly) | [Solder-free J-Link (recommended) / serial](a1-flash) |
| DAIKIN | BRY88AB151K supports [stock OTA](daikin-stock-ota) | [ESP32 serial installation](daikin-guide#first-installation) |
| S30-V1B | [Stock 2M.1220 network installation](s30-flash#network-installation-from-stock-firmware) | [Serial installation and recovery](s30-flash#disassembly-and-uart) |

## First Wi-Fi setup

This section describes community firmware. Devices still running factory firmware should use [M1 factory setup](m1-no-disassembly) / [A1 factory setup](a1-no-disassembly) first.

If Wi-Fi has not been configured, the device opens a setup access point. Connect a phone or computer and open `http://192.168.0.1/`.

| Device | Setup access point |
| --- | --- |
| TC1, A1 hardware revision | `TC1-AP-<last six MAC digits>` |
| M1 | `M1-AP-<last six MAC digits>` |
| DAIKIN | `BRY88AX151K-AP-<last six MAC digits in uppercase>` |
| A1 air purifier | `A1-AP-<last six MAC digits>` |
| S30-V1B | `S30M_V1B_<last six MAC digits>` |

All setup AP names use uppercase MAC digits. Older firmware may use a different hotspot name or setup address; check its release notes.

## Open the device WebUI

After setup, find the device's DHCP address in your router and open that IP in a browser. The top bar offers Chinese / English and light / dark themes. Settings are grouped into network, device, automation and maintenance; each product shows only supported functions. Save changes explicitly. Some network changes disconnect the browser.

Theme preferences are stored locally in the browser and shared with the device UI on that origin.

## Next steps

- Daily operation: [TC1](tc1-guide), [M1](m1-guide), [A1](a1-guide), [DAIKIN](daikin-guide), [S30-V1B](s30-guide).
- [Firmware updates](firmware-update).
- [Connect to IoTCloud](iotcloud-connect) for remote device management.
- [MQTT, Home Assistant and A1 sensor automation](mqtt-link).
- [Troubleshooting](faq).
