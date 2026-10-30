# Update the M1 controller over the network

You can update M1 controller firmware over the network **without J-Link or opening the case**. The current method uses a Python script on a computer that can reach M1 over the LAN.

**This updates the ATSAMD20G18A sensor controller for sensors, the screen, buttons and buzzer.** Use the Python tool below. Ordinary web firmware updates only update the EMW3080 Wi-Fi module.

**Check the controller version first: migration is recommended for stock 11.x. Stock 17.1.2.3 / 17.x differs substantially and migration is discouraged.** Wi-Fi and controller versions are separate. If the controller version is unknown, identify the hardware and original version first.

<!-- os-picker -->

## 1. Check Wi-Fi firmware

Stock 11.x controllers require community Wi-Fi firmware with **buffered image forwarding**. Check `data.mcu_ota_max_bytes` in `capabilities.get` or `/api/v1/capabilities`; the current limit is **32768**. For the v1 test procedure, use **M1 Wi-Fi v1.0.0-rc.3 with sensor controller v1.0.0-rc**. RC builds are for beta testing; use stable Wi-Fi v0.9.27 for everyday operation. Still check this capability before updating the sensor controller. The per-frame bridge introduced in v0.9.26 and the `1.0.0-39a5b0` build do not include this capability.

Stock 11.1.2.1 starts an approximately ten-second reset timer when receiving begins. Per-frame HTTP round trips can exceed that window. The new endpoint buffers the whole image on Wi-Fi first, then sends and acknowledges each UART block locally. The updated computer tool selects it automatically and refuses stock-controller upgrades through older Wi-Fi before sending any update frames. Controllers already running community firmware can still use the older per-frame bridge.

Web controls for RTTTL music, the four notification tones and alarm tunes require **Wi-Fi v0.9.27 or newer** and a separately flashed controller v0.9.3 or newer with matching capabilities. Particle details require controller v0.9.4 capabilities. Installing Wi-Fi alone does not enable these features. See [controller features](m1-mcu-firmware).

## 2. Download the image and tools

Download these two files from the [MCU v1.0.0-rc Release](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/releases/tag/v1.0.0-rc), keep their names and place them in the same directory:

- `m1-mcu-ota.bin`
- `m1-mcu-ota.json`

Both must come from the same Release. The script verifies image length, SHA-256, chip target, application address and manifest. SHA256SUMS and debugging/recovery files are also available, but network flashing does not use the recovery package or the full stock backup.

Install Python 3. Use the pinned tool version **`01af1ebccdaf48b73d02eaaf7023f68d5a0d280f`**, which supports buffered forwarding for stock controllers. **The tools in the MCU v1.0.0-rc tag do not contain this fix.** Tool and image versions are separate: keep using `m1-mcu-ota.bin` and its JSON from the Release above; do not rebuild or substitute the image.

```sh
git clone https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20.git
cd phicomm_m1_mcu_atsamd20
git checkout --detach 01af1ebccdaf48b73d02eaaf7023f68d5a0d280f
```

Alternatively, download the [source ZIP for this tool version](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/archive/01af1ebccdaf48b73d02eaaf7023f68d5a0d280f.zip), extract it and enter the source directory. The script uses only the Python standard library; no pip dependencies are needed. Keep `tools/pack_ota.py` alongside `tools/m1_network.py` rather than downloading the latter alone.

## 3. Enter your device details

Find the current IP in your router or M1 web interface, and obtain the device ID from device information. The ID verifies the target device; it is not an arbitrary name. Use the new address if the IP changes.

Replace `DEVICE_IP`, `DEVICE_ID` and `/download-directory/` below with your own values. Run the commands from the controller source directory.

Only if a web password is configured, set it first:

<!-- os:windows -->

```powershell
$env:M1_WEB_PASSWORD='your web password'
```

<!-- /os -->

<!-- os:mac linux -->

```sh
export M1_WEB_PASSWORD='your web password'
```

<!-- /os -->

Windows commands below use one line and `C:/Downloads/` as an example directory. Replace it with the folder containing your downloaded image and JSON; keep paths quoted if they contain spaces.

## 4. Check without flashing

<!-- os:windows -->

```powershell
py -3 tools/m1_network.py --host DEVICE_IP --device-id DEVICE_ID --image "C:/Downloads/m1-mcu-ota.bin" --manifest "C:/Downloads/m1-mcu-ota.json"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 tools/m1_network.py \
  --host DEVICE_IP \
  --device-id DEVICE_ID \
  --image /download-directory/m1-mcu-ota.bin \
  --manifest /download-directory/m1-mcu-ota.json
```

<!-- /os -->

Without `--flash`, no firmware is written. Check `device_id`, `before_mcu`, `candidate`, `sha256`, `bytes` and `mcu_ota_max_bytes`. This step checks the local image and reads device identity and controller version. Flashing still requires the Wi-Fi version and capabilities listed above.

## 5. Flash the application

After checking the result, add `--flash` to the same arguments:

<!-- os:windows -->

```powershell
py -3 tools/m1_network.py --host DEVICE_IP --device-id DEVICE_ID --image "C:/Downloads/m1-mcu-ota.bin" --manifest "C:/Downloads/m1-mcu-ota.json" --flash
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 tools/m1_network.py \
  --host DEVICE_IP \
  --device-id DEVICE_ID \
  --image /download-directory/m1-mcu-ota.bin \
  --manifest /download-directory/m1-mcu-ota.json \
  --flash
```

<!-- /os -->

Keep power connected. Do not simultaneously update Wi-Fi, change settings or hold the body button. The controller update restarts Wi-Fi and preserves its settings. The tool waits up to about 120 seconds and checks the controller version after reconnection. See [controller and Wi-Fi reset](m1-mcu-firmware#how-the-stock-controller-handles-provisioning-and-wi-fi-reset).

The **MCU v1.0.0-rc** package should report **`1.0.0-rc-97a03b`**. Wait for `Verified MCU runtime` and check that the version matches.

If runtime confirmation times out, wait for the device to reconnect and check power, IP, identity and actual controller version before repeating. For `HTTP 401`, check the web password. If the bridge returns `404`, check the Wi-Fi version. If busy, wait for conflicting operations to finish.

## What is updated

This procedure **updates only the controller application linked at 0x4000 and preserves the stock Bootloader**. It does not install the independent new Boot or its added CRC journal/interrupted-copy recovery guarantees. Do not use a full stock backup or Boot image as application OTA payload.

Use an image and manifest from the same Release, and keep power connected during the update. See [controller features and stock backup](m1-mcu-firmware) and the [network update protocol](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/blob/main/docs/network-ota.md).
