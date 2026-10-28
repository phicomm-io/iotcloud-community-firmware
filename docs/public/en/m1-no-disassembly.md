# M1 installation without disassembly: LAN upload and DNS migration

M1 can join your router through its factory hotspot, then accept community firmware from a computer without the original app. This procedure updates the Wi-Fi module and keeps the original main MCU program.

**The first migration from factory firmware must use 0.9.21 or newer.** Choose the **full M1 native OTA image, `phicomm_m1.ota.bin.raw`**. Older versions can repeatedly restart during first boot; do not install an old version as a stepping stone. This guide pins downloads to 0.9.21.

This guide covers factory LAN uploads and self-hosted DNS migration for the M1 Wi-Fi module. After installation, confirm the version, web access and sensors in step 5. Use the separate installation guide for an A1 purifier.

Select your computer system to show the matching commands. Replace example device IPs with the address in your router's client list.

<!-- os-picker -->

## 1. Prepare the local network

Connect the computer to the same router as M1 over Ethernet or Wi-Fi, with client isolation disabled. No public device ports are needed. For stalled uploads, see the self-hosted DNS migration in section 6. There is no public migration DNS address to enter directly.

## 2. Connect the factory M1

If the router already lists M1, keep its configuration and record the IP and label MAC. No reset is needed.

To provision again, **hold the body button for about 3–4 seconds, then release**, and find the open `EasyLink_XXXXXX` hotspot. Join it and submit your **2.4 GHz** network settings to `10.0.0.1:8000/config-write-uap`. See [stock Wi-Fi setup](stock-provision) for the full procedure. No original app is required.

A short press switches the screen display. **Holding about 9 seconds and then releasing restores factory network and application settings and reboots.** For ordinary provisioning, release after 3–4 seconds.

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

First migration may require Wi-Fi setup again. If the old IP is unreachable after upload, scan for the community hotspot.

| Stage | Hotspot | Access while joined |
| --- | --- | --- |
| Factory setup | Open `EasyLink_XXXXXX` | `http://10.0.0.1:8000/config-read` |
| Community first boot | Open `M1-AP-XXXXXX` | `http://192.168.0.1/`, port 80 |
| Community on the LAN | Find the new IP in the router | `http://device-IP/`, port 80 |

1. Join the `M1-AP-XXXXXX` matching your device's MAC suffix. Stay connected even if your computer reports no Internet.
2. Open `http://192.168.0.1/` and save your 2.4 GHz Wi-Fi settings in the initial setup page. Allow time to connect.
3. Rejoin the router and find M1 by its label MAC. The new IP may match the factory IP or change. Open `http://device-IP/` using port 80, rather than the factory port 8000.
4. Confirm model M1, version **0.9.21 or newer**, updating temperature/humidity and PM2.5 readings, and normal display and device communication.
5. After installation and connectivity are confirmed, a normal power cycle can check boot, automatic reconnection and retained settings.

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

## 6. DNS migration: let the stock M1 download the image

Use this alternative if LAN uploads stall. On 2026-10-26, one M1 with stock Wi-Fi `14.2.4.1` and controller `11.1.2.1` successfully migrated to full build `1.0.0-39a5b0`. Complete download, community hotspot, web access, Wi-Fi setup, automatic reconnection after a software reboot and updating sensor readings were verified. A physical cold boot was not tested. This 1.0.0 build identifier does not imply a formal Release exists or that every stock revision has been validated.

Uploads to `:8000/OTA` from two computers stalled after the first few KB; the device-initiated download completed. The upload fault remains unexplained. HTTP 000 or a slowly blinking Wi-Fi icon does not establish success. Do not repeatedly upload.

### What you need

- A stock M1 connected and identified using sections 2–3. Reserve its DHCP address on the router so it stays unchanged during migration.
- An OpenWrt / ImmortalWrt router with SSH access, `dnsmasq` and `nft`, on the same LAN as M1 and the computer.
- A Linux computer with Python 3 and `openssl`, kept running throughout. Run the server and router commands in Linux / router shells. Windows and macOS users can use a Linux host on the same LAN.
- A **full native M1 OTA image** with a verified SHA-256, selected as in section 4. Do not use `.all.bin`, `.signed` or a controller image. Native source-build output may be named `phicomm_m1.ota.bin`, while the published download may end in `.ota.bin.raw`; validate its contents rather than converting by renaming.
- Download the [migration server script](assets/tools/m1-dns-migration/m1_dns_migration.py) and [SHA256SUMS](assets/tools/m1-dns-migration/SHA256SUMS). Run `sha256sum -c SHA256SUMS` in their directory.

Examples use router `192.168.1.1`, computer `192.168.1.10`, M1 `192.168.1.123` and M1 MAC `00:11:22:33:44:55`. **Replace every example with your own values.** Computer ports `29080`, `29443`, `29000` and router port `5533` must be free. Allow the device and router to reach those computer ports through any firewall.

### Step 1: start the temporary server on the computer

Place the script and image in one directory. Set `--sha256` to this image's value from a trusted checksum manifest:

```sh
python3 m1_dns_migration.py \
  --image ./phicomm_m1.ota.bin.raw \
  --sha256 'REPLACE_WITH_64_HEX_SHA256_FROM_TRUSTED_MANIFEST' \
  --mac 00:11:22:33:44:55 \
  --device-ip 192.168.1.123 \
  --router-ip 192.168.1.1 \
  --server-ip 192.168.1.10
```

Keep the window open after `"kind": "ready"`. The script validates SHA-256, native M1 product identity, length, CRC and trailing MD5 before serving. It stores its certificate and private key in `m1-migration-state/`. Activation and update responses are restricted to the selected MAC; **no `mainSoftware` update is returned**. Activation uses the MAC as a temporary device ID, not an original account binding or restored factory cloud service.

The temporary HTTPS endpoint supports older factory clients and generates a self-signed certificate valid for two days. Use it only on your own LAN, stop it after migration and do not expose it to the Internet.

### Step 2: route only this M1's DNS requests on the router

SSH into the router and run these commands in the same shell. They leave `/etc/hosts`, global DNS and DHCP settings unchanged. A separate resolver handles only the selected M1's redirected DNS requests.

```sh
RouterIp=192.168.1.1
ServerIp=192.168.1.10
DeviceIp=192.168.1.123
mkdir -p /tmp/m1-dns-migration
cp /etc/hosts /tmp/m1-dns-migration/hosts.before
uci export dhcp > /tmp/m1-dns-migration/dhcp.before
nft list ruleset > /tmp/m1-dns-migration/nft.before

dnsmasq --conf-file=/dev/null --no-hosts \
  --port=5533 --listen-address="$RouterIp" --bind-interfaces \
  --no-resolv --server=127.0.0.1#53 \
  --host-record="aircat.phicomm.com,$RouterIp" \
  --host-record="phiclouds.phicomm.com,$RouterIp" \
  --pid-file=/tmp/m1-dns-migration/dns.pid \
  --log-queries --log-facility=/tmp/m1-dns-migration/dns.log

cat > /tmp/m1-dns-migration/rules.nft <<EOF_RULES
table ip m1_dns_migration {
 chain prerouting {
  type nat hook prerouting priority -110; policy accept;
  ip saddr $DeviceIp udp dport 53 counter redirect to :5533
  ip saddr $DeviceIp tcp dport 53 counter redirect to :5533
  ip saddr $DeviceIp ip daddr $RouterIp tcp dport 443 counter dnat to $ServerIp:29443
  ip saddr $DeviceIp ip daddr $RouterIp tcp dport 9000 counter dnat to $ServerIp:29000
 }
 chain postrouting {
  type nat hook postrouting priority 101; policy accept;
  ip saddr $DeviceIp ip daddr $ServerIp tcp dport { 29443, 29000 } counter snat to $RouterIp
 }
}
EOF_RULES
nft -c -f /tmp/m1-dns-migration/rules.nft && nft -f /tmp/m1-dns-migration/rules.nft
```

The rules use a dedicated table. If it already exists, clean up the previous session using step 5 before restarting. Other domains continue through the router's normal resolver. Port 9000 only receives factory status reports; the script sends no control commands.

Adding two global `/etc/hosts` entries can affect other devices and still does not provide HTTPS compatibility, an update manifest or firmware downloads. The per-device rules above connect the complete migration path.

### Step 3: reboot M1 and observe its own requests

Confirm the server is ready and no upload or flash operation is active, then disconnect M1 power for about 10 seconds and reconnect. Keep power and networking stable. Watch the computer output and the router log:

```sh
tail -f /tmp/m1-dns-migration/dns.log
```

Expected computer events are:

1. `tls` and `/device/active?mac=…&productId=1`: the stock device reaches the compatible service.
2. `/firmware/checkUpdate?...wifiSoftwareVer=...&mainSoftwareVer=...`: the device checks for updates. Its original controller must return its version.
3. `/m1-native.bin?deviceMAC` and `download_complete`: the complete image was sent. Query strings and Range requests are supported.
4. `wifiSoftware.ifSuccess: true` inside `cloud_tcp`: the stock application reports update success. This does not prove community firmware has booted.

Manifest version `14.2.4.2` is only a comparison value that makes the tested stock `14.2.4.1` recognize an update; **it is not the community image version**. Verify the actual installed version after boot. If requests are absent, check connectivity, DNS-rule counters, server ports and the controller, rather than repeatedly rebooting or forcing different files.

### Step 4: verify community firmware and set up Wi-Fi

Wait a few minutes, scan for `M1-AP-XXXXXX` and follow section 5 to open `192.168.0.1` and configure 2.4 GHz Wi-Fi. Find the new DHCP address. Check `/api/v1/status` for the full `device_id`, actual `version`, `wifi_connected`, `sensor_valid` and increasing `rx_frames`. Verify the web page and automatic reconnection with retained settings after a software reboot.

A complete download or factory success report alone is insufficient. Keep power connected and check the hotspot, leases and version before sending anything again.

### Step 5: restore routing and stop the temporary service

After migration is verified, or after the test has stopped with no transfer / flash operation remaining, run on the router:

```sh
nft delete table ip m1_dns_migration
kill "$(cat /tmp/m1-dns-migration/dns.pid)"
cmp /etc/hosts /tmp/m1-dns-migration/hosts.before
uci export dhcp > /tmp/m1-dns-migration/dhcp.after
cmp /tmp/m1-dns-migration/dhcp.before /tmp/m1-dns-migration/dhcp.after
```

No output from the two `cmp` commands confirms these original configurations did not change. Keep the logs; do not overwrite the entire router configuration with old snapshots. Press `Ctrl+C` in the computer's server window. You may then remove its temporary certificate directory. Future updates use community signed OTA and do not need this service. The M1 hardware result does not establish A1 compatibility.

## 7. Troubleshooting

| Symptom | Action |
| --- | --- |
| Empty reply / HTTP 000 | Check whether upload completed, keep power connected and check the community hotspot and version before retrying |
| Neither the old IP nor expected hotspot works | The community name is `M1-AP-XXXXXX`; check router leases and scan results rather than only the old IP |
| Community hotspot exists, old IP fails | Join the hotspot and set up Wi-Fi at `192.168.0.1` |
| Upload stalls after a few KB | Let the request finish; after stock access is restored, use device-initiated downloading in section 6 instead of repeated uploads |
| Blinking icon with a DHCP lease | Verify HTTP access; the icon may reflect the factory cloud connection |
| No web access, hotspot or client record after waiting | Keep image hashes and upload output, stop repeated writes and read boot logs using the [serial / J-Link recovery guide](a1-m1-flash) |

This procedure only updates the Wi-Fi module. The sensor controller uses a [separate update procedure](m1-mcu-update).
