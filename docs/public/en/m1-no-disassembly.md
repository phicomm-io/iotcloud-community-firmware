# M1 no-disassembly flashing: LAN upload and public DNS migration

M1 can join your router through its factory hotspot, then accept community firmware from a computer without the original app. This procedure updates the Wi-Fi module and keeps the original main MCU program.

**The first migration from factory firmware must use 0.9.21 or newer.** Choose the **full M1 native OTA image, `phicomm_m1.ota.bin.raw`**. Older versions can repeatedly restart during first boot; do not install an old version as a stepping stone. This guide pins downloads to 0.9.21.

A fix candidate passed factory LAN OTA, community hotspot web access, Wi-Fi setup and sensor communication on one M1. Still verify the actual boot result of the published package. Public DNS migration remains untested and no ready-to-use migration DNS is supplied. These M1 results do not establish A1 compatibility.

Select your computer system to show the matching commands. Replace example device IPs with the address in your router's client list.

<!-- os-picker -->

## 1. Choose a path

| Path | User action | Status |
| --- | --- | --- |
| LAN | Upload a native OTA image to the factory configuration service | M1 fix candidate passed; first migration starts at 0.9.21 |
| Public service | Router DNS directs M1 to a compatible image service | Service, TLS and full migration still need device validation |

Use the LAN path. Your computer can reach that router over Ethernet or Wi-Fi. Do not expose device ports to the Internet.

## 2. Connect the factory M1

If the router already lists M1, keep its configuration and record the IP and label MAC. No reset is needed.

Otherwise enter factory provisioning and confirm an open `EasyLink_XXXXXX` hotspot appears. Join it and submit your **2.4 GHz** network settings to `10.0.0.1:8000/config-write-uap`. See [stock Wi-Fi setup](stock-provision) for the entry limits and system-specific commands. No original app is required.

The window is about two minutes. Once the hotspot disappears, reconnect your computer to the router and match the new DHCP lease to the label MAC. Blinking Wi-Fi, an X icon or a disappearing hotspot alone does not prove connection. Avoid repeatedly pressing the pairing button.

## 3. Identify the LAN target

Replace the example IP and keep this terminal open for subsequent commands:

<!-- os:windows -->

```powershell
$DeviceIp = "192.168.1.123"
curl.exe --noproxy "*" --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
DeviceIp="192.168.1.123"
curl --noproxy '*' --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

Match the response name and MAC suffix against the device label and router record. Resolve IP changes, failed provisioning or client isolation before uploading.

## 4. Download 0.9.21, verify and upload once

Download these two **0.9.21** files into an empty folder and open a terminal there:

- [Full M1 native OTA image]({{SITE_URL}}releases/v0.9.21/phicomm_m1.ota.bin.raw)
- [SHA256SUMS]({{SITE_URL}}releases/v0.9.21/SHA256SUMS)

**Check that the model is M1 and the version is at least 0.9.21.** Use the full image specified here for first migration, rather than `no-frontend-resources`. A full Flash backup or `.all.bin` is for other recovery procedures; `.ota.bin.signed` is for devices already running community firmware. None is the factory upload body. Renaming a file to `.raw` does not convert it.

Compute SHA-256 and compare it character for character with the **`phicomm_m1.ota.bin.raw` line** in the checksum file. Stop on a mismatch.

<!-- os:windows -->

```powershell
Get-FileHash -Algorithm SHA256 .\phicomm_m1.ota.bin.raw
Select-String -Path .\SHA256SUMS -Pattern '\s+\*?phicomm_m1\.ota\.bin\.raw$'
```

<!-- /os -->

<!-- os:mac -->

```sh
shasum -a 256 ./phicomm_m1.ota.bin.raw
awk '$2 == "phicomm_m1.ota.bin.raw" { print }' ./SHA256SUMS
```

<!-- /os -->

<!-- os:linux -->

```sh
sha256sum ./phicomm_m1.ota.bin.raw
awk '$2 == "phicomm_m1.ota.bin.raw" { print }' ./SHA256SUMS
```

<!-- /os -->

Keep power and connectivity stable. Upload once using HTTP/1.0, without a proxy, Expect handshake or automatic retries:

<!-- os:windows -->

```powershell
curl.exe --http1.0 --noproxy "*" --connect-timeout 5 --max-time 300 --header "Content-Type: application/ota-stream" --header "Expect:" --data-binary "@phicomm_m1.ota.bin.raw" --write-out "uploaded=%{size_upload} http=%{http_code}\n" "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
curl --http1.0 --noproxy '*' --connect-timeout 5 --max-time 300 --header 'Content-Type: application/ota-stream' --header 'Expect:' --data-binary '@phicomm_m1.ota.bin.raw' --write-out 'uploaded=%{size_upload} http=%{http_code}\n' "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

**The tested factory upload closed the connection after receiving the complete image, producing `(52) Empty reply from server` and `http=000`.** This alone proves neither failure nor success. Record the uploaded byte count, leave power connected and allow about 2–3 minutes. Check the community hotspot and version in section 5 before taking further action. Do not immediately resend, reset or disconnect power because of this message.

## 5. Join the community hotspot and finish setup

Factory network settings may not carry over. If the old IP is unreachable after upload, scan for the community hotspot. Wi-Fi had to be configured again in the tested migration.

| Stage | Hotspot | Access while joined |
| --- | --- | --- |
| Factory setup | Open `EasyLink_XXXXXX` | `http://10.0.0.1:8000/config-read` |
| Community first boot | Open `M1-AP-XXXXXX` | `http://192.168.0.1/`, port 80 |
| Community on the LAN | Find the new IP in the router | `http://device-IP/`, port 80 |

1. Join the `M1-AP-XXXXXX` matching your device's MAC suffix. Stay connected even if your computer reports no Internet.
2. Open `http://192.168.0.1/` and save your 2.4 GHz Wi-Fi settings in the initial setup page. Allow time to connect.
3. Rejoin the router and find M1 by its label MAC. The new IP may match the factory IP or change. Open `http://device-IP/` using port 80, rather than the factory port 8000.
4. Confirm model M1, version **0.9.21 or newer**, updating temperature/humidity and PM2.5 readings, and normal display and device communication.
5. After installation and connectivity are confirmed, a normal power cycle can check boot, automatic reconnection and retained settings. The candidate test did not cover this step; keep your own result.

For a read-only status check, update the address in the same terminal:

<!-- os:windows -->

```powershell
$DeviceIp = "192.168.1.123"
curl.exe --noproxy "*" --connect-timeout 3 --max-time 5 "http://${DeviceIp}/api/v1/status"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
DeviceIp="192.168.1.123"
curl --noproxy '*' --connect-timeout 3 --max-time 5 "http://${DeviceIp}/api/v1/status"
```

<!-- /os -->

A community hotspot establishes that the new system offers setup. Confirm the version, web access and updating sensors before calling migration successful. For later community upgrades, use signed packages as described in [firmware updates](firmware-update).

## 6. Public migration through a compatible service

```text
Connected device → controlled DNS → compatible activation / state handling
→ factory update check → image URL and MD5 → download and install → verified new runtime
```

The backup references these endpoints. Their current availability has not been checked.

| Purpose | Factory endpoint |
| --- | --- |
| Activation | `https://phiclouds.phicomm.com/device/active?mac=…&productId=1` |
| Update check | `https://aircat.phicomm.com/firmware/checkUpdate?mac=…&wifiSoftwareVer=…&mainSoftwareVer=…` |
| Business connection | `aircat.phicomm.com:9000` |

### Configure and check DNS

Proceed only after a maintainer supplies a DNS address and migration image validated on the target hardware. No such address is supplied by this guide.

1. Record the router's current DNS and device network settings.
2. Configure the supplied DNS in the router's DHCP / LAN settings. If the router proxies DNS, follow the service instructions for forwarding too. Changing only the computer's DNS does not change the device's resolver.
3. Follow router instructions to renew the device lease and resolution; never restart during firmware transfer or writing. A device using static DNS needs separate confirmation.
4. Query the update hostname from a computer on that LAN. Compare with the maintainer's expected result, then confirm the target device's own requests in service logs.

<!-- os:windows -->

```powershell
Resolve-DnsName aircat.phicomm.com -Type A
```

<!-- /os -->

<!-- os:mac linux -->

```sh
dig +short A aircat.phicomm.com
```

<!-- /os -->

The computer's result does not establish the device's resolver or migration success. Restore the user's original DNS after migration.

### Response format for service maintainers

These values illustrate the parser structure. They are not a deployable update manifest:

```json
{
  "wifiSoftware": {
    "url": "http://migration.example/m1-native.bin",
    "ver": "14.2.4.2",
    "md5": "<32 hexadecimal MD5 characters of the actual response body>",
    "enforce": true
  }
}
```

Provide only `wifiSoftware` for Wi-Fi migration. Do not include `mainSoftware`, which targets the separate MCU. The remote Wi-Fi version string must differ from the local value and enforcement conditions must be satisfied. This is different from A1's numeric comparison. The example version is factory comparison metadata, not the image's community version.

The startup OTA thread waits for the main MCU's version report, requesting it again after a five-second timeout. Update checks therefore need a working MCU, although an app action is not necessarily required. The downloader appends a MAC query parameter and has Range/resume paths; the server must support the actual device requests.

The server must implement required activation/state exchanges, HTTPS update responses and actual image delivery. Compute MD5 from the bytes served. MD5 is a content checksum, not a factory private-key signature. The traced application path does not establish that arbitrary images will boot, nor replace a full bootloader audit.

The update-check TLS call disables CA verification, but legacy TLS versions, cipher suites, certificate algorithms and SNI behavior still require device testing. The downloader recognizes HTTP and HTTPS URLs; browser download success does not prove device compatibility.

Before public use, demonstrate activation, update triggering, TLS, complete download, installation, reboot and functional checks on the target factory device. A static file host plus changed DNS is insufficient. Until this is done, there is no verified “change DNS and reboot” procedure.

## 7. Troubleshooting

| Symptom | Action |
| --- | --- |
| Empty reply / HTTP 000 | Check whether upload completed, keep power connected and check the community hotspot and version before retrying |
| Neither the old IP nor expected hotspot works | The community name is `M1-AP-XXXXXX`; check router leases and scan results rather than only the old IP |
| Community hotspot exists, old IP fails | Join the hotspot and set up Wi-Fi at `192.168.0.1` |
| Blinking icon with a DHCP lease | Verify HTTP access; the icon may reflect the factory cloud connection |
| No web access, hotspot or client record after waiting | Keep image hashes and upload output, stop repeated writes and read boot logs using the [serial / J-Link recovery guide](a1-m1-flash) |
| DNS change does not trigger an update | Public migration remains unverified; inspect the device's actual resolver, service logs and trigger conditions |

The tested path is M1 Wi-Fi module migration over the LAN. MCU flashing, A1 migration and public DNS migration are outside that result.
