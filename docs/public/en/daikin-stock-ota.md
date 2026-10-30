# Daikin BRY88AB151K: migrate through stock OTA

Choose your computer’s operating system below. Checking the file and uploading will both show matching commands. On Windows, use PowerShell; on macOS or Linux, use Terminal. Without JavaScript, all commands remain visible.

<!-- os-picker -->

## 1. Prepare the device and computer

Use a stable device power supply and a computer with 2.4GHz Wi-Fi and Python 3. The sender needs only the Python standard library. Download the files before switching the computer to the device hotspot. Ethernet can keep the computer online while Wi-Fi connects to the device, but internet access is not required during flashing.

Have a recovery method available. For a first attempt, keep your own complete 4MB serial Flash backup. Another device's complete backup can overwrite identity and private settings.

Stock OTA listens on TCP 8555 after entering update mode. It writes the inactive application slot, selects it for boot, and restarts. **Entering OTA mode already initializes and erases the inactive slot.** Keep power connected.

## 2. Download and check the files

- [Python sender](../assets/tools/daikin-stock-ota/daikin_stock_ota.py)
- [Tested BRY88AB151K application](../assets/tools/daikin-stock-ota/daikin-bry88ab.application.bin)
- [SHA256SUMS](../assets/tools/daikin-stock-ota/SHA256SUMS)

Save the sender and image in the same directory. The application must be **1,027,296 bytes**, with SHA-256:

```text
649a7b81b35e19510f9f7edb8488157418117849361bc62c6f4e13c61a5fb19e
```

This is the unchanged ordinary `application.bin`. Do not send `factory.bin`, a complete Flash backup, a signed wrapper, or an image for another model. Each stock application slot is **1,572,864 bytes (1.5MiB)**. That limit remains after migration.

Open a terminal in the download directory and check the file without contacting the device.

<!-- os:windows -->

Windows PowerShell:

```powershell
py -3 .\daikin_stock_ota.py .\daikin-bry88ab.application.bin --sha256 649a7b81b35e19510f9f7edb8488157418117849361bc62c6f4e13c61a5fb19e --check-only
```

<!-- /os -->

<!-- os:mac linux -->

macOS / Linux:

```sh
python3 ./daikin_stock_ota.py ./daikin-bry88ab.application.bin --sha256 649a7b81b35e19510f9f7edb8488157418117849361bc62c6f4e13c61a5fb19e --check-only
```

<!-- /os -->

Install Python 3 first if the command is unavailable. Stop on a hash or format mismatch and download the correct file again.

## 3. Trigger stock OTA with the button

With the stock application running, locate the **RESET pinhole on the back of the device**. Use a suitable thin tool to gently press the button inside, then follow this sequence:

1. Hold for approximately **3 seconds**, then release for approximately **0.5 seconds**.
2. Hold for approximately **3 seconds**, then release for approximately **0.5 seconds**.
3. Hold for approximately **3 seconds**, then release.

Each hold must fall between approximately 2 and 5 seconds. Keep each release-to-next-press gap below 2 seconds. Do not replace the sequence with one long hold or hold beyond 5 seconds. This sequence started the mode 0 OTA task on the test device.

**In some stock provisioning states, a long hold clears settings and restarts the device instead.** The code checks an internal `wififlag`; this gesture is not guaranteed to preserve settings in every mode. If the device restarts or the gesture fails, observe its state rather than repeatedly extending the hold time.

If a serial console is available, success includes:

```text
Start OTA Task Suceess, mode 0
app_ota_server_create success
```

Serial access is useful for diagnosis but is not required for the wireless transfer itself.

## 4. Connect to the stock hotspot

Select the device's `daikin!` hotspot using the computer's Wi-Fi menu. For this stock version, the default hotspot password is **`12345678`**.

The tested SSID was `daikin!f76748f9`; the suffix varies by device. Check your own device identity. The tested STA MAC was `9c:9c:1f:f7:67:48` and its AP BSSID was `9c:9c:1f:f7:67:49`.

The test computer received `192.168.4.2/24` through DHCP, with device/gateway **`192.168.4.1`**. Keep this connection even if the OS reports no internet. Do not reuse the device's old router-assigned address, and prevent automatic switching to another Wi-Fi network.

On Windows, use the Wi-Fi menu when possible. In the remote SSH test, a current-user `netsh` profile returned `0x57`; an all-users profile for this hotspot worked. Do not delete unrelated network profiles.

## 5. Probe the accepted version label

Stock firmware accepts only a version label higher than its current version. The old fixed `.16` label therefore fails on newer devices. Stay on the device network and probe an explicit range without an image:

<!-- os:windows -->

```powershell
py -3 .\daikin_stock_ota.py --probe-versions SV.02.00.01.16 SV.02.00.01.32
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 ./daikin_stock_ota.py --probe-versions SV.02.00.01.16 SV.02.00.01.32
```

<!-- /os -->

The scan waits **2 seconds** between attempts and stops immediately on `ToDo`, an unexpected response, a timeout or another communication error. It sends only handshake/version headers: **0 firmware bytes**, no `End` or checksum, no Wi-Fi switching and no automatic upload.

For example, if `.16` through `.20` return `b'No' / EOF` and `.21` returns `b'ToDo' / complete`, use `.21` as the first accepted label in the upload command.

Use the first accepted label explicitly in the separate upload command below; change the example `.21` to your result. If communication failed, resolve it before trying another command.

## 6. Upload the application

Confirm that the computer is connected to your device's hotspot, then run the sender. Its default destination is `192.168.4.1:8555`.

<!-- os:windows -->

Windows PowerShell:

```powershell
py -3 .\daikin_stock_ota.py .\daikin-bry88ab.application.bin --sha256 649a7b81b35e19510f9f7edb8488157418117849361bc62c6f4e13c61a5fb19e --version SV.02.00.01.21
```

<!-- /os -->

<!-- os:mac linux -->

macOS / Linux:

```sh
python3 ./daikin_stock_ota.py ./daikin-bry88ab.application.bin --sha256 649a7b81b35e19510f9f7edb8488157418117849361bc62c6f4e13c61a5fb19e --version SV.02.00.01.21
```

<!-- /os -->

`--version` is the stock protocol admission label, not the community firmware version; it does not modify the image. The uploader requires this explicit choice and never reuses a probe result automatically. Confirm the image's model, layout and SHA-256 independently.

Expected output includes:

```text
Kit2 Version Up -> Okey
SV.02.00.01.21 -> ToDo
ACK .../1027296 (...%)
Final response: b'OK'
```

Wait for the final `OK` and reboot. The sender waits for each length ACK and separates `End` from the final checksum. This service is not an HTTP upload endpoint: do not replace the sender with `curl` or combine protocol messages into one write.

## 7. Verify the community application

After reboot, connect to the new open hotspot **`DAIKIN-XXXXXX`**, using the last six MAC digits. The tested hotspot became `DAIKIN-f76748`. An old SSID can remain in scan caches briefly.

Open **http://192.168.4.1** and verify the model, device ID and version. The tested device reported `BRY88AB151K`, `9c9c1ff76748`, and `0.9.21-2cfd89-dirty`. Configure router Wi-Fi, a management password and MQTT as needed.

The transition image included with this guide uses the older hotspot name and address above. After updating to v0.9.25, the recovery hotspot uses `BRY88AX151K-AP-<last six MAC digits in uppercase>` at `http://192.168.0.1/`. Once router Wi-Fi is configured, continue using the router-assigned IP for normal access.

Check readings after sensor warm-up. CO₂ may take longer; devices without a formaldehyde sensor have no HCHO reading.

This procedure retains the stock bootloader and partition table and starts the community application from the second slot, `0x190000`. Configure Wi-Fi again on first boot. Future images must still fit the stock 1.5 MiB application slots.

## 8. Troubleshooting and recovery

| Symptom | Action |
| --- | --- |
| No hotspot | Check model/version, normal stock boot and gesture timing. The OTA task does not necessarily create an AP in every configuration state. |
| Hotspot connects but TCP 8555 times out/refuses | Check network switching and the actual gateway. Inspect the two OTA startup log markers if available. |
| Handshake or version rejected | Use the handshake-only scan in section 5. Select an accepted label explicitly for upload, and check image compatibility separately. |
| Transfer interrupted, invalid ACK or final NG | Save the output and observe the powered device. Wait for reboot and confirm the community version before another update. |
| OK but no new hotspot | Wait for reboot, rescan and inspect the device startup logs. |
| Device will not boot | Stop repeated wireless update attempts and seek serial recovery using your own complete Flash backup. |
