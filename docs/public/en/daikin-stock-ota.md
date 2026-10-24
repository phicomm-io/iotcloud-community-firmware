# Daikin BRY88AB151K: migrate through stock OTA

On 2026-10-24, a **BRY88AB151K running stock SV.02.00.01.15 with 4MB Flash** successfully accepted a community application through its stock TCP OTA service and booted it. This guide uses an ordinary ESP32 application image; no separate stock-OTA build is needed.

**Do not use this image on BRY88AX151K or assume other stock versions are compatible.** The test device was backed up and restored to stock over serial before wireless migration. The result validates the button gesture and OTA path, not every factory configuration state. The provided image is the tested `0.9.21-2cfd89-dirty` test build, not a claim about the current stable release.

## 1. Prepare the device and computer

Use a stable device power supply and a computer with 2.4GHz Wi-Fi and Python 3. The sender needs only the Python standard library. Download the files before switching the computer to the device hotspot. Ethernet can keep the computer online while Wi-Fi connects to the device, but internet access is not required during flashing.

Have a recovery method available. For a first attempt, keep your own complete 4MB serial Flash backup. Another device's complete backup can overwrite identity and private settings.

Stock OTA listens on TCP 8555 after entering update mode. It writes the inactive application slot, selects it for boot, and restarts. **Entering OTA mode already initializes and erases the inactive slot.** Keep power connected. GPIO0 is for serial download mode; the wireless gesture uses the pairing button mapped to GPIO34.

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

Windows PowerShell:

```powershell
py -3 .\daikin_stock_ota.py .\daikin-bry88ab.application.bin --sha256 649a7b81b35e19510f9f7edb8488157418117849361bc62c6f4e13c61a5fb19e --check-only
```

macOS / Linux:

```sh
python3 ./daikin_stock_ota.py ./daikin-bry88ab.application.bin --sha256 649a7b81b35e19510f9f7edb8488157418117849361bc62c6f4e13c61a5fb19e --check-only
```

Install Python 3 first if the command is unavailable. Stop on a hash or format mismatch and download the correct file again.

## 3. Trigger stock OTA with the button

With the stock application running, use the pairing button:

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

## 5. Upload the application

Confirm that the computer is connected to your device's hotspot, then run the sender. Its default destination is `192.168.4.1:8555`.

Windows PowerShell:

```powershell
py -3 .\daikin_stock_ota.py .\daikin-bry88ab.application.bin --sha256 649a7b81b35e19510f9f7edb8488157418117849361bc62c6f4e13c61a5fb19e
```

macOS / Linux:

```sh
python3 ./daikin_stock_ota.py ./daikin-bry88ab.application.bin --sha256 649a7b81b35e19510f9f7edb8488157418117849361bc62c6f4e13c61a5fb19e
```

The default stock protocol version label is `SV.02.00.01.16`. It passes the tested stock version check; it does not change the image or represent the community version. When using another image in future, use its actual publisher-provided SHA-256 and separately verify model and stock-layout compatibility.

Expected output includes:

```text
Kit2 Version Up -> Okey
SV.02.00.01.16 -> ToDo
ACK .../1027296 (...%)
Final response: b'OK'
```

Wait for the final `OK` and reboot. The sender waits for each length ACK and separates `End` from the final checksum. This service is not an HTTP upload endpoint: do not replace the sender with `curl` or combine protocol messages into one write.

## 6. Verify the community application

After reboot, connect to the new open hotspot **`DAIKIN-XXXXXX`**, using the last six MAC digits. The tested hotspot became `DAIKIN-f76748`. An old SSID can remain in scan caches briefly.

Open **http://192.168.4.1** and verify the model, device ID and version. The tested device reported `BRY88AB151K`, `9c9c1ff76748`, and `0.9.21-2cfd89-dirty`. Configure router Wi-Fi, a management password and MQTT as needed.

PM2.5, temperature, humidity and TVOC had valid readings after boot. CO2 was still warming up; HCHO was not detected and had also reported no-data errors under stock. Successful transfer does not prove every sensor works.

Serial logs confirmed that the stock bootloader and partition table remained and the community application booted from the second stock slot, `0x190000`. Stock network settings do not automatically become community configuration. Configuration persistence in the smaller stock NVS, power-cycle retention and a second community OTA were not verified in this test. Future images must still fit the stock 1.5MiB slots.

## 7. Troubleshooting and recovery

| Symptom | Action |
| --- | --- |
| No hotspot | Check model/version, normal stock boot and gesture timing. The OTA task does not necessarily create an AP in every configuration state. |
| Hotspot connects but TCP 8555 times out/refuses | Check network switching and the actual gateway. A hotspot alone does not prove the OTA service started; inspect the two serial markers if available. |
| Handshake or version rejected | Stop and check the stock version and compatibility. Do not invent a higher label to bypass compatibility checks. |
| Transfer interrupted, invalid ACK or final NG | Save the output and observe the powered device. Acknowledged bytes alone do not prove successful flashing. |
| OK but no new hotspot | Wait for reboot, rescan and inspect serial logs. Ensure GPIO0 was released after any earlier serial work. |
| Device will not boot | Restore your own complete Flash backup through ESP32 serial download mode. GPIO0/EN recovery is separate from the wireless button gesture. |

The verified result covers this stock version on BRY88AB151K migrating to the fixed application provided here. Other models, configuration states and later updates require separate verification.
