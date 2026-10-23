# A1 no-disassembly flashing: LAN upload and public DNS migration

The factory firmware has a LAN OTA upload interface and a cloud update path. Once connected, the device can use these paths for a compatible community migration image.

**This is a guide awaiting physical verification. The interface paths are confirmed from a backup, but no image verified through factory installation and boot is supplied here, and there is no ready-to-use public migration DNS. Complete provisioning and read-only checks first; stop before uploading until a verified compatible image is available.**

This A1 is the air purifier, not the TC1 A1 hardware revision. Select Windows PowerShell, macOS or Linux to view the matching commands.

The analyzed factory version is A1 `v1.47`; other versions require separate interface and image checks.

<!-- os-picker -->

## 1. Choose a path

| Path | User action | Prerequisites |
| --- | --- | --- |
| LAN | Upload a native OTA image from a computer on the same LAN | Compatible image and device IP |
| Public service | Use router DNS to direct the device to a compatible update service | Validated service, controlled DNS, compatible image and factory update conditions |

LAN upload avoids cloud protocol reproduction and update scheduling. A DNS change only changes name resolution; a working migration service must exist first. The device initiates public-service connections; do not expose port 8000 to the Internet.

## 2. Connect A1

If A1 already appears in the router client list, preserve its settings. A compatible network with its saved SSID and password may also allow reconnection.

Otherwise, connect the computer over the target 2.4 GHz Wi-Fi, download and extract the [EasyLink tool bundle](assets/tools/a1-easylink.zip), enter its `a1-easylink` folder and start the local tool:

<!-- os:windows -->

```powershell
py -3 .\a1_easylink_web.py
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 ./a1_easylink_web.py
```

<!-- /os -->

Enter the credentials and Wi-Fi adapter IPv4 in the local page. Sending requires A1 to be listening. Its touch input has an approximately one-second threshold, but the matching physical icon remains unverified. Follow the [factory Wi-Fi setup guide](stock-provision) for the full procedure and limitations.

## 3. Identify the LAN target

Connect the computer by Wi-Fi or Ethernet to the same router. Obtain the device IP from its client list and replace this example:

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

Match the response with the target device and router MAC record. Resolve IP changes or network isolation before proceeding. This path receives firmware bytes directly; TC1's TFTP trigger, extra Ethernet address and JSON `FILENAME` do not apply.

## 4. Check the image and upload once

Obtain an image specifically validated for A1's factory native OTA path, targeting community version **0.9.21 or newer**. Meeting the version minimum still requires factory-install compatibility validation. Its publisher must identify supported factory versions, SHA-256, first-install procedure and post-boot access.

A full Flash backup, `.all.bin` or community `.signed` wrapper cannot be used directly as the request body. A `.raw` suffix alone is not proof of compatibility. The name `a1-migration-native.bin` is an example, not a supplied download.

Compare the image's SHA-256 with its publisher's value:

<!-- os:windows -->

```powershell
Get-FileHash -Algorithm SHA256 .\a1-migration-native.bin
```

<!-- /os -->

<!-- os:mac -->

```sh
shasum -a 256 ./a1-migration-native.bin
```

<!-- /os -->

<!-- os:linux -->

```sh
sha256sum ./a1-migration-native.bin
```

<!-- /os -->

Keep power and connectivity stable and upload once:

<!-- os:windows -->

```powershell
curl.exe --noproxy "*" --connect-timeout 5 --max-time 300 --header "Content-Type: application/ota-stream" --header "Expect:" --data-binary "@a1-migration-native.bin" "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
curl --noproxy '*' --connect-timeout 5 --max-time 300 --header 'Content-Type: application/ota-stream' --header 'Expect:' --data-binary '@a1-migration-native.bin' "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

The factory handler writes the OTA staging partition, accumulates CRC16, saves update information and requests a restart. No automatic retry is configured. If the connection closes or times out, inspect the device and router before sending again.

## 5. Public migration through a compatible service

```text
Connected device → controlled DNS → compatible activation / state handling
→ factory update check → image URL and MD5 → download and install → verified new runtime
```

The backup references these endpoints. Their current availability has not been checked.

| Purpose | Factory endpoint |
| --- | --- |
| Activation | `https://phicloudsym.phicomm.com/device/active?mac=…&productId=30` |
| Update check | `https://aircleaner.phicomm.com/aircleanerfirmware/checkUpdate` |
| Business / OTA connection | `aircleanerconnecter.phicomm.com:9000`, with a separate legacy-address branch |

### Configure and check DNS

Proceed only after a maintainer supplies a DNS address and migration image validated on the target hardware. No such address is supplied by this guide.

1. Record the router's current DNS and device network settings.
2. Configure the supplied DNS in the router's DHCP / LAN settings. If the router proxies DNS, follow the service instructions for forwarding too. Changing only the computer's DNS does not change the device's resolver.
3. Follow router instructions to renew the device lease and resolution; never restart during firmware transfer or writing. A device using static DNS needs separate confirmation.
4. Query the update hostname from a computer on that LAN. Compare with the maintainer's expected result, then confirm the target device's own requests in service logs.

<!-- os:windows -->

```powershell
Resolve-DnsName aircleaner.phicomm.com -Type A
```

<!-- /os -->

<!-- os:mac linux -->

```sh
dig +short A aircleaner.phicomm.com
```

<!-- /os -->

The computer's result does not establish the device's resolver or migration success. Restore the user's original DNS after migration.

### Response format for service maintainers

These values illustrate the parser structure. They are not a deployable update manifest:

```json
{
  "url": "http://migration.example/a1-native.bin",
  "md5": "<32 hexadecimal MD5 characters of the actual response body>",
  "ver": "v1.48",
  "enforce": true
}
```

The factory parser removes the version's first character and compares its numeric value with local `1.47`. The remote value must be greater, enforcement enabled and no OTA already active. This metadata version is not the community image's version. Do not add `device_id`, which can select the activation-response branch.

The hourly timer also checks activation/business state, clock synchronization and an hour field equal to `18`. That does not guarantee an immediate update at the user's local 18:00. The separate TCP OTA command path has not been reproduced with its complete session and framing; an isolated JSON command is not a verified shortcut.

The server must implement required activation/state exchanges, HTTPS update responses and actual image delivery. Compute MD5 from the bytes served. MD5 is a content checksum, not a factory private-key signature. The traced application path does not establish that arbitrary images will boot, nor replace a full bootloader audit.

The update-check TLS call disables CA verification, but legacy TLS versions, cipher suites, certificate algorithms and SNI behavior still require device testing. The downloader recognizes HTTP and HTTPS URLs; browser download success does not prove device compatibility.

Before public use, demonstrate activation, update triggering, TLS, complete download, installation, reboot and functional checks on the target factory device. A static file host plus changed DNS is insufficient. Until this is done, there is no verified “change DNS and reboot” procedure.

## 6. Verify the migration

1. Confirm the expected model and community firmware version after restart, using the image publisher's access instructions.
2. Verify Wi-Fi, local access and purifier controls; check whether configuration retention matches the migration contract.
3. Power cycle after installation has completed and confirm boot, network and operation again.

HTTP success, complete file delivery or a disconnected session are intermediate observations. Keep update-check, download and confirmed runtime states separate in service logs.

## 7. Troubleshooting

| Symptom | Check |
| --- | --- |
| Configuration endpoint unreachable | Current IP, successful provisioning, device identity and client isolation |
| No requests after a DNS change | Actual device resolver, cache, activation and update-trigger conditions |
| Check succeeds but download does not | Model-specific manifest, version comparison and enforcement |
| Browser downloads but device fails | Legacy TLS, exact query/Range requests and response-body MD5 |
| Download completes but device does not boot | Preserve file hashes and service evidence; do not treat download as installation |

No device was connected or flashed while preparing this guide.
