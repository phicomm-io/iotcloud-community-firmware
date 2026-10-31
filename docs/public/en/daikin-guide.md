# DAIKIN functions, installation and recovery

The ESP32 / 4 MB Flash board uses one universal image that detects PM2005 / PM2105 and the optional formaldehyde module. Its web interface shares M1 reading cards, comfort scales and linked-device information, with local access, IoTCloud and Home Assistant support.

**Use stable v0.9.27 for everyday operation, or v1.0.0-rc.5 for beta testing.** See the [complete candidate release notes](release-log.html?channel=candidate) for version requirements and cumulative changes.

## Readings and controls

- Temperature and humidity use the model recovered from this unit’s stock firmware: temperature is reduced by 4.9 °C; humidity is corrected through the mixing ratio and corrected temperature, then bounded to 0–100%. In General settings, customize the temperature subtraction (default 4.9 °C) and humidity adjustment (default 0 %RH), or restore defaults. Saved values apply immediately and survive restart. Positive temperature offsets lower readings; negative offsets raise them. Humidity follows the corrected temperature.
- Read PM2.5, PM10, CO₂, temperature, humidity and TVOC. PM2005 / PM2105 provide both particulate readings in one frame; the interface displays separate cards in one row; PM10 stays hidden when it is unavailable. The CO₂ module needs about three minutes to warm up; the interface reports no valid reading during this period.
- Supporting firmware adds Settings → General → CO₂ module diagnostics and calibration, with raw readings, status bytes, communication errors and calibration results. The current legacy compatibility profile retains reserved status bytes without decoding fault flags, which remain unknown. Manual calibration requires a known reference concentration of 400–1500 ppm, stable for at least two minutes. Outdoor air is not necessarily 400 ppm. After calibration, compare subsequent readings against the reference. A missing acknowledgement is reported as an unknown outcome and is not retried automatically.
- Formaldehyde is detected automatically. Without the module, its card and entity stay hidden. A previously detected module that stops responding becomes invalid; old readings never become zero measurements.
- Comfort scales follow fresh readings. Stale values do not contribute to comfort ratings. TVOC uses the module's native ppb unit, with a ventilation reference of ≤300 ppb independent of custom indicator thresholds. Particulate display bands follow HJ 633—2026 table 3; these instantaneous readings are not a calculated AQI.
- Configure the name, network, web password and indicators in Settings. Hold the rear button for five seconds to reopen the setup hotspot without clearing settings.

The OLED and internal GPIO26 indicator are currently unused. BRY88AA151K has different wiring and ADC TVOC and is unsupported by this image.

## Indicator settings

Open Settings → General → Indicator rules and enable the air quality indicators. CO₂, TVOC and PM each support up to two thresholds with steady on, always off, fast blink or slow blink. Select either PM2.5 or PM10 as the PM lamp's data source; only the selected measurement controls that lamp. Formaldehyde rules are available when its module is detected. Rules survive a restart. The highest matching threshold wins within a measurement; unmatched or stale readings turn its output off. TVOC / formaldehyde share one lamp, with priority given to fast blink, slow blink, steady on, then off.

The Wi-Fi lamp indicates system status: three long and three short pulses when unconfigured; fast blink when Wi-Fi is disconnected (150 ms on/off); slow blink when configured MQTT is not ready (1 s on/off); steady on when ready. MQTT can remain unconfigured without preventing readiness. This lamp is independent of the air quality indicator switch.

## First installation

Choose a release that **explicitly includes DAIKIN** on the [downloads page](../../index.html#releases). Do not substitute another model's image when an older archive lists only Phicomm devices. Existing 0.9.x archives provide the full image only. The current v1.0.0-rc.5 also provides a separate `hub` variant as a beta prerelease.

1. Confirm the ESP32 / 4 MB Flash board and wiring, then back up the entire original Flash. Keep that backup private because it contains network credentials.
2. Hold GPIO0 / BOOT0 low while powering on to enter the ESP32 bootloader. Use ESPTool to write `daikin_BRY88AX151K.all.bin` at **0x0**. Never upload the factory image through web OTA.
3. Release BOOT0 and power cycle. Join `BRY88AX151K-AP-<last six MAC digits in uppercase>` (for example, `BRY88AX151K-AP-AABBCC`) and open `http://192.168.0.1/` to configure Wi-Fi. The hotspot uses the same name and address when reopened by the button or after a network failure.
4. Find the device IP in your router and check the local readings. Allow CO₂ to warm up; no formaldehyde card appears without its module.

Use ESP32-compatible 3.3 V serial signals and avoid back-powering the device from a USB serial adapter. Review [Before flashing](flash-prep) if wiring is uncertain. A complete image or backup restore overwrites Flash; confirm the device and file first.

## Updates and recovery

Historical releases may still use the `phicomm_daikin` prefix. Use the filenames listed on that release’s download page and in `SHA256SUMS`.

Upload `daikin_BRY88AX151K.ota.bin.signed` from the firmware update page. The device verifies the project signature, target and image before installing; Wi-Fi and MQTT settings remain. Updates currently use manual files; URL downloads and slim packages are unavailable.

An explicitly labelled unsigned recovery input accepts a trusted raw application image. Releases name it `daikin_BRY88AX151K.ota.bin.raw`; local builds use `daikin_BRY88AX151K.ota.bin`. If the web interface is unavailable, enter the BOOT0 bootloader again and restore the original backup or a complete image. See [Firmware updates](firmware-update).

## IoTCloud, Home Assistant and A1

Follow [Connect to IoTCloud](iotcloud-connect) using an MQTT account on the selected node. The device is discovered as **DAIKIN BRY88AX151K**, and the platform provides the same reading cards as its local interface. Home Assistant discovery publishes only sensors that are present.

Add this device's MAC and optional IP in the updated A1 settings. MQTT under the same account grants reading sessions automatically. A1 prefers local reads and uses MQTT measurements when local reads are unavailable. The monitor's linked-purifier area shows A1 devices that have actually read it; a grant alone is not a connection.

A1 offers automation sources according to currently valid data, independently of the monitor model. Missing formaldehyde or stale readings hide the corresponding options while preserving saved rules. If required control data is unavailable, A1 uses its configured offline speed. TVOC is not currently a fan-control metric.

## Hub edition

**The hub is a beta feature provided in a separate `hub` image.** See the [release notes](release-log.html?channel=candidate) for matching versions. Update source devices, M1 receivers and the frontend together; the old sensor interface is incompatible.

The hub discovers devices through mDNS, communicates over paired UDP, and forwards upstream MQTT and Bemfa messages. Local Home Assistant and MQTT clients use an external broker. The full edition remains a sensor device.

Select **Hub management** on the device page, or open `http://DEVICE-IP/iotcloud/#/`. The hub uses the device web administration password and session. With no password, it opens directly. With a password, sign in once; switching between the control and hub pages in the same tab reuses the session. Change the password in device settings, and select **Sign out** to end the session.

After upgrading to rc.5, the separate hub password is retired. The existing hub username, device list, forwarding and pairing settings are retained. The hub username remains the topic namespace. Configure UDP device-pairing passwords and external MQTT broker credentials separately.

The DAIKIN itself appears automatically in the hub device list. Paired-device parameters and controls use the hub UDP transport, so the browser only needs to reach the hub. TC1 cards show power and hide expired meter readings. Remote pages expose supported controls and diagnostics. Open the target device’s local page for network settings, administration passwords, automation, upgrades and backups.

In Hub management, select a discovered LAN device, check its model, ID and IP, and pair using that device's management password. Confirm and pair again if its IP changes. Move devices previously connected to the hub's embedded broker to an external broker.

Apple Home uses experimental HAP bridging, off by default. Enable it under **Apple Home** to obtain the pairing code. Matter is unavailable.

### Switch from full to hub

Only newer `full` firmware that advertises this operation provides the conversion option. The image must fit the device's actual OTA slot. Use local HTTP and a real signed `hub` OTA file; 0.9.x full images do not provide this feature.

1. Open the device on your LAN. A web administration password is optional; if one is configured, use it to authenticate.
2. Open **Settings → Firmware → Advanced**, select the matching signed `hub` OTA file and enable **Switch to hub firmware**. This option cannot be combined with unsigned recovery.
3. Select **Upload and install**, review the confirmation and explicitly approve the conversion. Wi-Fi, MQTT and the web administration password are retained.
4. Reopen the device after restart, confirm its variant is `hub`, select **Hub management**, and reuse the same web administration session if a password is configured. HomeKit is off by default; enable it separately when needed.

Normal signed updates and unsigned recovery still accept only the current variant. Direct OTA from `hub → full` is unsupported.
