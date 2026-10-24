# DAIKIN BRY88AX151K air quality monitor

The ESP32 / 4 MB Flash board uses one universal image that detects PM2005 / PM2105 and the optional formaldehyde module. Its web interface shares M1 reading cards, comfort scales and linked-device information, with local access, IotCloud and Home Assistant support.

## Readings and controls

- Read PM2.5, PM10, CO₂, temperature, humidity and TVOC. PM2005 / PM2105 provide both particulate readings in one frame; the interface displays separate cards in one row; PM10 stays hidden when it is unavailable. The CO₂ module needs about three minutes to warm up; the interface reports no valid reading during this period.
- Formaldehyde is detected automatically. Without the module, its card and entity stay hidden. A previously detected module that stops responding becomes invalid; old readings never become zero measurements.
- Comfort scales follow fresh readings. Stale values do not contribute to comfort ratings. TVOC uses the module's native ppb unit, with a ventilation reference of ≤300 ppb matching the device indicator. Particulate display bands follow HJ 633—2026 table 3; these instantaneous readings are not a calculated AQI.
- Configure the name, network, web password and indicators in Settings. Hold the rear button for five seconds to reopen the setup hotspot without clearing settings.

The OLED and internal GPIO26 indicator are currently unused. BRY88AA151K has different wiring and ADC TVOC, so compatibility with that board is not established.

## First installation

Choose a release that **explicitly includes DAIKIN** on the [downloads page](../../index.html#releases). Do not substitute another model's image when an older archive lists only Phicomm devices. This device has a full image only.

1. Confirm the ESP32 / 4 MB Flash board and wiring, then back up the entire original Flash. Keep that backup private because it contains network credentials.
2. Hold GPIO0 / BOOT0 low while powering on to enter the ESP32 bootloader. Use ESPTool to write `phicomm_daikin.all.bin` at **0x0**. Never upload the factory image through web OTA.
3. Release BOOT0 and power cycle. Join `DAIKIN-<last six MAC digits>` and open `http://192.168.4.1/` to configure Wi-Fi.
4. Find the device IP in your router and check the local readings. Allow CO₂ to warm up; no formaldehyde card appears without its module.

Use ESP32-compatible 3.3 V serial signals and avoid back-powering the device from a USB serial adapter. Review [Before flashing](flash-prep) if wiring is uncertain. A complete image or backup restore overwrites Flash; confirm the device and file first.

## Updates and recovery

Upload `phicomm_daikin.ota.bin.signed` from the firmware update page. The device verifies the project signature, target and image before installing; Wi-Fi and MQTT settings remain. Updates currently use manual files; URL downloads and slim packages are unavailable.

An explicitly labelled unsigned recovery input accepts a trusted raw application image. Releases name it `phicomm_daikin.ota.bin.raw`; local builds use `phicomm_daikin.ota.bin`. If the web interface is unavailable, enter the BOOT0 bootloader again and restore the original backup or a complete image. See [Firmware updates](firmware-update).

## IotCloud, Home Assistant and A1

Follow [Connect to IotCloud](iotcloud-connect) using an MQTT account on the selected node. The device is discovered as **DAIKIN BRY88AX151K**, and the platform provides the same reading cards as its local interface. Home Assistant discovery publishes only sensors that are present.

Add this device's MAC and optional IP in the updated A1 settings. MQTT under the same account grants reading sessions automatically. A1 prefers local reads and uses MQTT measurements when local reads are unavailable. The monitor's linked-purifier area shows A1 devices that have actually read it; a grant alone is not a connection.

A1 offers automation sources according to currently valid data, independently of the monitor model. Missing formaldehyde or stale readings hide the corresponding options while preserving saved rules. If required control data is unavailable, A1 uses its configured offline speed. TVOC is not currently a fan-control metric.
