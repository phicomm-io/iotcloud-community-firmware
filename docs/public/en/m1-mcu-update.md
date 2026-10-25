# Update the M1 controller over the network

You can update M1 controller firmware over the network **without J-Link or opening the case**. The current method uses a Python script on a computer that can reach M1 over the LAN.

**This updates the sensor controller, not the Wi-Fi MCU.** EMW3080 runs Wi-Fi and the web interface; ATSAMD20G18A communicates with sensors and controls the screen and buzzer. To reduce accidental wrong-image flashing, the sensor controller has no WebUI upload button. Use the computer script below: it calls `POST /api/v1/mcu/exchange` and verifies the image and runtime version. Ordinary web firmware updates only update Wi-Fi.

**Check the controller version first: migration is recommended for stock 11.x. Stock 17.1.2.3 / 17.x differs substantially and migration is discouraged.** Wi-Fi and controller versions are separate. If the controller version is unknown, identify the hardware and original version first.

<!-- os-picker -->

## 1. Check Wi-Fi firmware

The minimum publicly released compatible community **Wi-Fi firmware is v0.9.26**, which introduces the `POST /api/v1/mcu/exchange` bridge. Stock Wi-Fi firmware and community v0.9.24 or earlier do not support this procedure. v0.9.25 has a source tag but no public Release, so it is not the download baseline.

Check the Wi-Fi version in the M1 web interface. If it is older, install [M1 Wi-Fi v0.9.26 or newer]({{SITE_URL}}#releases) using the ordinary Wi-Fi updater, wait for the device to reconnect, then update the controller. These are separate version sequences: v0.9.26 here means **Wi-Fi firmware**, while v0.9.4 below means **controller firmware**.

Web controls for RTTTL music, the four notification tones and alarm tunes require **Wi-Fi v0.9.27 or newer** and a separately flashed controller v0.9.3 or newer with matching capabilities. Particle details require controller v0.9.4 capabilities. Installing Wi-Fi alone does not enable these features. See [controller features](m1-mcu-firmware).

## 2. Download the image and tools

Download these two files from the [MCU v0.9.4 Release](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/releases/tag/v0.9.4), keep their names and place them in the same directory:

- `m1-mcu-ota.bin`
- `m1-mcu-ota.json`

Both must come from the same Release. The script verifies image length, SHA-256, chip target, application address and manifest. SHA256SUMS and debugging/recovery files are also available, but network flashing does not use the recovery package or the full stock backup.

Install Python 3. To obtain the matching tools using Git:

```sh
git clone --depth 1 --branch v0.9.4 https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20.git
cd phicomm_m1_mcu_atsamd20
```

Alternatively, download Source code ZIP from the Release, extract it and open a terminal in the source directory. The script uses only the Python standard library; no pip dependencies are needed. Keep `tools/pack_ota.py` alongside `tools/m1_network.py` rather than downloading the latter alone.

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

Without `--flash`, no firmware is written. Check `device_id`, `before_mcu`, `candidate`, `sha256` and `bytes`. This validates the local image and reads device identity and controller version. **It does not send update frames or prove that the bridge endpoint is supported**, so the Wi-Fi version prerequisite still applies.

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

Keep power connected. Do not simultaneously update Wi-Fi, reboot or change configuration. The script waits for each block acknowledgment and reports progress. Controller reboot temporarily disconnects Wi-Fi; the tool then waits approximately 120 seconds at most for reconnection and a fresh matching controller version from the same device.

The **MCU v0.9.4** package should report **`0.9.4-1ce539`**. `Verified MCU runtime` with the matching version confirms the post-update runtime. `Commit transmitted` alone only confirms that the commit was sent.

If runtime confirmation times out, wait for the device to reconnect and check power, IP, identity and actual controller version before repeating. For `HTTP 401`, check the web password. If the bridge returns `404`, check the Wi-Fi version. If busy, wait for conflicting operations to finish.

## What is updated

This procedure **updates only the controller application linked at 0x4000 and preserves the stock Bootloader**. It does not install the independent new Boot or its added CRC journal/interrupted-copy recovery guarantees. Do not use a full stock backup or Boot image as application OTA payload.

Use an image and manifest from the same Release, and keep power connected during the update. See [controller features and stock backup](m1-mcu-firmware) and the [network update protocol](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/blob/v0.9.4/docs/network-ota.md).
