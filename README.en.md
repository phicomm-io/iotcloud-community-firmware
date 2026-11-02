# IoTCloud community device firmware

[简体中文](README.md)

Community firmware for TC1 power strips, M1 air monitors, A1 air purifiers, DAIKIN air monitors and ORVIBO S30-V1B metering sockets. Use the device's local web interface, MQTT, Home Assistant or IoTCloud. Local use does not require a platform account.

**[Devices and features](https://phicomm-io.github.io/en/#devices) · [Install, update and downloads](https://phicomm-io.github.io/en/#install) · [Common questions](https://phicomm-io.github.io/en/#faq) · [Contribute](https://phicomm-io.github.io/en/#develop)**

## Devices and features

| Device | Everyday features | Install and use |
| --- | --- | --- |
| TC1 power strip, A1 hardware revision | Six outlets, power and energy, schedules, child lock and button controls | [Install](https://phicomm-io.github.io/guides/en/tc1-no-disassembly.html) · [Use](https://phicomm-io.github.io/guides/en/tc1-guide.html) |
| M1 air monitor | PM2.5, formaldehyde, temperature, humidity, brightness and A1 automation; matching open-source controller firmware adds particle details, CO₂ display and music | [Install](https://phicomm-io.github.io/guides/en/m1-no-disassembly.html) · [Use](https://phicomm-io.github.io/guides/en/m1-guide.html) |
| A1 air purifier | Fan, automatic mode, filter, light ring, night mode and multiple sensor providers; a fresh-air variant is also available | [Install](https://phicomm-io.github.io/guides/en/a1-no-disassembly.html) · [Use](https://phicomm-io.github.io/guides/en/a1-guide.html) |
| DAIKIN air monitor | PM2.5, PM10, CO₂, temperature, humidity, TVOC, optional formaldehyde and A1 automation | [Install and use](https://phicomm-io.github.io/guides/en/daikin-guide.html) |
| ORVIBO S30-V1B metering socket | Single outlet, voltage / current / power / energy, power rules and power-on policy | [Install](docs/public/en/s30-flash.md) · [Use](docs/public/en/s30-guide.md) |

TC1's A1 hardware revision and the A1 purifier are different products; their images are not interchangeable. TC1 A2 is unsupported. M1 / A1 require the original EMW3080B / MK3080B modules. Check the documented ESP32 / 4 MB Flash model for DAIKIN.

**M1's Wi-Fi module and sensor controller update separately.** The [GPL-3.0 open-source controller](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20) is recommended for stock **11.x**. Stock **17.1.2.3 / 17.x** differs substantially and awaits analysis; migration is currently discouraged. Read the actual controller version and leave unreadable versions unknown. Wi-Fi OTA does not update the controller; basic air readings do not require a controller upgrade. See [controller compatibility](https://phicomm-io.github.io/guides/en/m1-mcu-firmware.html).

## Install and update

1. Read [Before flashing](https://phicomm-io.github.io/guides/en/flash-prep.html). Check hardware, backups, power and recovery access.
2. Choose your model and variant from [existing downloads](https://phicomm-io.github.io/en/#releases), verify `SHA256SUMS` and follow its installation guide.
3. Use [Quick start](https://phicomm-io.github.io/guides/en/quick-start.html) to set up Wi-Fi and open the device IP. Optionally connect [MQTT / Home Assistant](https://phicomm-io.github.io/guides/en/mqtt-link.html) or [IoTCloud](https://phicomm-io.github.io/guides/en/iotcloud-connect.html).

Normal web updates on community firmware use the matching `.ota.bin.signed` package. A raw application `.ota.bin.raw` is only for migration or recovery procedures that explicitly accept it. The full `.all.bin` image is for wired installation and cannot be uploaded through web OTA. See [updates and recovery](https://phicomm-io.github.io/guides/en/firmware-update.html). S30-V1B uses its `.rbl` URL without signature verification or local file uploads; first installation follows the [network / UART guide](docs/public/en/s30-flash.md).

The DAIKIN stock OTA layout has **`0x180000` (1.5 MiB)** application slots. Follow the [stock OTA guide](https://phicomm-io.github.io/guides/en/daikin-stock-ota.html) for the first migration to normal firmware. For the signed `full → hub` conversion in v1.0.0-rc.5, see the [hub guide](https://phicomm-io.github.io/guides/en/daikin-guide.html#switch-from-full-to-hub). It requires supporting firmware and an image that fits the device.

## v1.0.0-rc.5 Beta candidate

**RC releases are beta builds. Major changes are still expected, and daily use is not recommended. Use stable [v0.9.27](https://phicomm-io.github.io/en/#releases) for everyday operation.** The current candidate is [v1.0.0-rc.5](https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/v1.0.0-rc.5). Interfaces and usage instructions use RC5 as their baseline. Wi-Fi firmware, the M1 sensor controller, frontend and platform are released separately; check their matching requirements.

| Area | Candidate features |
| --- | --- |
| Use and updates | Chinese / English interfaces, device language, configuration backup and restore, migration recovery and upgrade comparisons |
| M1 | Independent sources for the formaldehyde / PM2.5 positions: local sensors, TC1 power, A1 effective main-fan output (0–100%), DAIKIN CO₂ and other readings, or the API; double-press switches the original / user pair, with separate expiry fallback and threshold or fixed colors; steadier PM2.5 and separate over-threshold buzzer switches, off by default, independent of light thresholds, alarms and manual tones |
| A1 recovery | Wi-Fi touch sequences: two short taps, then hold the third touch for 1–2 seconds and release to reset networking; three short taps, then hold the fourth touch for 1–2 seconds and release to factory reset. Existing 5 / 10 second holds remain. The new sequences await physical acceptance |
| DAIKIN `hub` | Separate hub variant: local accounts, mDNS discovery, paired UDP device control, an upstream MQTT client and Bemfa forwarding, automatic local-device listing and an optional web password; normal firmware remains a sensor device |
| Interfaces and recovery | MessagePack over HTTP / MQTT, separate sensor metadata and live values, M1 profile4 configuration, configuration backup and recovery |

Apple Home uses experimental HomeKit HAP bridging on the hub. Pairing, controls and restart recovery await physical-device validation. Matter is not included.

## Build and contribute

Interfaces, protocols, configuration compatibility and test vectors are maintained in the [bilingual developer documentation](https://phicomm-io.github.io/iotcloud-community-protocols/en/). Read [CONTRIBUTING](CONTRIBUTING.md) and use the `feature/* → dev → main` workflow.

| Project | Scope |
| --- | --- |
| [iotcloud-community-firmware](https://github.com/phicomm-io/iotcloud-community-firmware) | Firmware, builds and user guides |
| [iotcloud-community-frontend](https://github.com/phicomm-io/iotcloud-community-frontend) | Device WebUI, IoTCloud and demos |
| [iotcloud-community-mqtt](https://github.com/phicomm-io/iotcloud-community-mqtt) | Platform service, MQTT and deployment |
| [iotcloud-community-protocols](https://github.com/phicomm-io/iotcloud-community-protocols) | Shared developer contracts |
| [phicomm_m1_mcu_atsamd20](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20) | M1 sensor controller |

Prepare the Linux toolchains and SDK using [Build dependencies](docs/public/en/build-deps.md), then run:

```sh
python3 -m pip install -r requirements-build.txt
./build.sh                  # TC1, M1 and A1
./DAIKIN/build.sh           # Normal DAIKIN firmware; requires PlatformIO
```

See [TC1](TC1/README.md), [M1](M1/README.md), [A1](A1/README.md) and [DAIKIN](DAIKIN/README.md) for individual builds and output files. `frontend.lock.json` pins the full frontend commit and SHA-256; `protocols.lock.json` pins shared contracts. Do not edit generated pages or demos.

User guide sources live in `docs/public/`; site layout sources live in `site/`. Check documentation with `python3 -m unittest discover -s tests -p test_site_docs.py`. Firmware changes require the tests and builds described in CONTRIBUTING.

## License

Project-maintained code uses [GPL-3.0](LICENSE). SDKs and third-party components retain their original copyright notices and terms. See the [third-party index](THIRD_PARTY_NOTICES.md).

Interfaces and runnable examples: [RC5 / 0.9.27 integration guide](docs/public/en/device-api.md).
