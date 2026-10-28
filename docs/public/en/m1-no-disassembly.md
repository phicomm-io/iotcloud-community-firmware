# M1: stock Wi-Fi setup and installation without disassembly

This guide updates only the M1 Wi-Fi module and retains the sensor controller. It covers two factory provisioning methods and two OTA methods. Prefer the official app for provisioning if available.

Use the **full native M1 OTA image**, version **0.9.21 or newer**. The commands below use stable **v0.9.27**. Keep power connected throughout.

<!-- os-picker -->

## 1. Provision stock Wi-Fi

If M1 is already listed by your router, preserve its settings and record its IP and MAC. Otherwise use one of the following methods. The target network must be 2.4 GHz WPA2/AES with DHCP; disable client isolation.

### Method A: hotspot HTTP setup

**Hold the body button for 3–4 seconds, then release.** Join the open `EasyLink_XXXXXX` hotspot (last six hexadecimal MAC digits) within about two minutes.

A short press changes the screen. Holding about **9 seconds and releasing restores factory network and application settings and restarts**. Ordinary provisioning uses the 3–4 second gesture.

### Connect and inspect

Join the target `EasyLink_XXXXXX` network. “No Internet” is expected. The factory configuration uses `10.0.0.1/24`, HTTP port 8000.

<!-- os:windows -->

```powershell
$DeviceIp = "10.0.0.1"
curl.exe --http1.0 --noproxy "*" --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
DeviceIp="10.0.0.1"
curl --http1.0 --noproxy '*' --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

If unreachable, check the active Wi-Fi network and its `10.0.0.x` address. The hotspot window may have expired. No TC1 TFTP server or secondary Ethernet address is needed.

### Submit Wi-Fi settings

Replace the SSID and password and run in the same terminal:

<!-- os:windows -->

```powershell
$Config = [ordered]@{
    SSID = "RescueWiFi"
    PASSWORD = "replace-with-your-wifi-password"
    DHCP = $true
    IDENTIFIER = 1
}
$ConfigFile = Join-Path $PWD.Path "m1-wifi.json"
$Json = $Config | ConvertTo-Json -Compress
[System.IO.File]::WriteAllText($ConfigFile, $Json, [System.Text.UTF8Encoding]::new($false))

curl.exe --http1.0 --noproxy "*" --connect-timeout 3 --max-time 10 --header "Content-Type: application/json" --header "Expect:" --data-binary "@$ConfigFile" "http://${DeviceIp}:8000/config-write-uap"
```

<!-- /os -->

<!-- os:mac linux -->

Create a UTF-8 JSON file. Escape any `"` or `\` in strings according to JSON rules:

```sh
umask 077
cat > m1-wifi.json <<'JSON'
{"SSID":"RescueWiFi","PASSWORD":"replace-with-your-wifi-password","DHCP":true,"IDENTIFIER":1}
JSON
curl --http1.0 --noproxy '*' --connect-timeout 3 --max-time 10 --header 'Content-Type: application/json' --header 'Expect:' --data-binary '@m1-wifi.json' "http://${DeviceIp}:8000/config-write-uap"
```

<!-- /os -->

Fields are case-sensitive and top-level. `DHCP` is a Boolean; `IDENTIFIER` is an integer used by the provisioning callback, not an account or binding token.

The factory path responds, waits about one second, closes the hotspot and attempts to join the router. Reconnect the computer to the router and look for M1's new DHCP lease. The file and terminal input records contain the password; use your own computer and delete the file when finished:

<!-- os:windows -->

```powershell
Remove-Item -LiteralPath $ConfigFile
```

<!-- /os -->

<!-- os:mac linux -->

```sh
rm -- m1-wifi.json
```

<!-- /os -->


### Method B: Linux EasyLink V2

App-free EasyLink has succeeded only with **mac80211 frame injection on Linux**. Prefer the official app if available. Ordinary Windows, macOS or Linux UDP senders are not verified provisioning methods.

Use a spare adapter supporting monitor mode and injection, Linux, `iw` and a C compiler. Use a 2.4 GHz WPA2/AES network and record its channel. Channel 1 has worked; channel 6 is not mandatory. Another adapter can keep the computer on the target LAN.

Download the [Linux injection sender source](assets/tools/easylink-linux/easylink_v2_linux.c) and [SHA256SUMS](assets/tools/easylink-linux/SHA256SUMS). Run these commands on Linux. Replace `phy1`, channel `1` and `192.168.1.23` with the spare radio, target channel and computer's IPv4 on the target LAN:

```sh
sha256sum -c SHA256SUMS
cc -O2 -Wall -Wextra -o easylink_v2_linux easylink_v2_linux.c
iw dev
sudo iw phy phy1 interface add easylink-mon type monitor
sudo ip link set easylink-mon up
sudo iw dev easylink-mon set channel 1
sudo ./easylink_v2_linux easylink-mon 192.168.1.23 60
```

Enter the target SSID and password when prompted (password is hidden). Once sending starts, hold M1's body button for 3–4 seconds and release. After sending, remove the temporary monitor interface:

```sh
sudo iw dev easylink-mon del
```

Confirm a DHCP lease matching the device MAC before continuing. Packet counts do not establish successful provisioning.

## 2. Identify the LAN target

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

## 3. Method A: upload through the factory LAN OTA interface

Download these two **0.9.27** files into an empty folder and open a terminal there:

- [Full M1 native OTA image]({{SITE_URL}}releases/v0.9.27/phicomm_m1.ota.bin.raw)
- [SHA256SUMS]({{SITE_URL}}releases/v0.9.27/SHA256SUMS)

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

## 4. Method B: DNS migration: let the stock M1 download the image

Use this alternative if LAN uploads stall. This path has been verified on M1 with stock Wi-Fi `14.2.4.1` and controller `11.1.2.1`.

Uploads to `:8000/OTA` from two computers stalled after the first few KB; the device-initiated download completed. The upload fault remains unexplained. HTTP 000 or a slowly blinking Wi-Fi icon does not establish success. Do not repeatedly upload.

### What you need

- A stock M1 connected and identified using sections 1–2. Reserve its DHCP address on the router so it stays unchanged during migration.
- An OpenWrt / ImmortalWrt router with SSH access, `dnsmasq` and `nft`, on the same LAN as M1 and the computer.
- A Linux computer with Python 3 and `openssl`, kept running throughout. Run the server and router commands in Linux / router shells. Windows and macOS users can use a Linux host on the same LAN.
- A **full native M1 OTA image** with a verified SHA-256, selected as in section 3. Do not use `.all.bin`, `.signed` or a controller image. Native source-build output may be named `phicomm_m1.ota.bin`, while the published download may end in `.ota.bin.raw`; validate its contents rather than converting by renaming.
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

No output from the two `cmp` commands confirms these original configurations did not change. Keep the logs; do not overwrite the entire router configuration with old snapshots. Press `Ctrl+C` in the computer's server window. You may then remove its temporary certificate directory. Future updates use community signed OTA and do not need this service.

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

## 6. Troubleshooting

| Symptom | Action |
| --- | --- |
| Empty reply / HTTP 000 | Check whether upload completed, keep power connected and check the community hotspot and version before retrying |
| Neither the old IP nor expected hotspot works | The community name is `M1-AP-XXXXXX`; check router leases and scan results rather than only the old IP |
| Community hotspot exists, old IP fails | Join the hotspot and set up Wi-Fi at `192.168.0.1` |
| Upload stalls after a few KB | Let the request finish; after stock access is restored, use device-initiated downloading in section 4 instead of repeated uploads |
| Blinking icon with a DHCP lease | Verify HTTP access; the icon may reflect the factory cloud connection |
| No web access, hotspot or client record after waiting | Keep image hashes and upload output, stop repeated writes and read boot logs using the [serial / J-Link recovery guide](m1-flash) |

This procedure only updates the Wi-Fi module. The sensor controller uses a [separate update procedure](m1-mcu-update).
