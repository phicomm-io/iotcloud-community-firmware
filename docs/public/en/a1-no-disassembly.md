# A1: stock Wi-Fi setup and installation without disassembly

For the A1 air purifier with an original EMW3080B / MK3080B module. **Factory LAN OTA is confirmed working.** This guide covers EasyLink provisioning, LAN upload and a DNS download path derived from stock firmware.

Use the **full native A1 OTA image**. Commands below use stable **v0.9.27**. Keep power connected and the filter installed. For wired recovery see [A1 flashing](a1-flash).

<!-- os-picker -->

## 1. Provision stock Wi-Fi

If your router already lists A1, preserve the configuration and record its IP and MAC. Otherwise use EasyLink. Prefer the official app if available.

Hold only the panel's **Wi-Fi icon button for 1–2 seconds**, then release; reception lasts about 60 seconds. This stock path listens for wireless packets and does not create a provisioning hotspot.

### Linux EasyLink V2 provisioning

App-free EasyLink uses **mac80211 frame injection on Linux** and requires a wireless adapter that supports it. The official app is another option.

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

Enter the target SSID and password when prompted (password is hidden). Once sending starts, hold only A1's Wi-Fi icon button for 1–2 seconds and release. After sending, remove the temporary monitor interface:

```sh
sudo iw dev easylink-mon del
```

Confirm a DHCP lease matching the device MAC before continuing.

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

- [Full A1 native OTA image]({{SITE_URL}}releases/v0.9.27/phicomm_a1.ota.bin.raw)
- [SHA256SUMS]({{SITE_URL}}releases/v0.9.27/SHA256SUMS)

**Check that the model is A1 and the version is at least 0.9.27.** Use the full image specified here for first migration, rather than `no-frontend-resources`. A full Flash backup or `.all.bin` is for other recovery procedures; `.ota.bin.signed` is for devices already running community firmware. None is the factory upload body. Renaming a file to `.raw` does not convert it.

Compute SHA-256 and compare it character for character with the **`phicomm_a1.ota.bin.raw` line** in the checksum file. Stop on a mismatch.

<!-- os:windows -->

```powershell
Get-FileHash -Algorithm SHA256 .\phicomm_a1.ota.bin.raw
Select-String -Path .\SHA256SUMS -Pattern '\s+\*?phicomm_a1\.ota\.bin\.raw$'
```

<!-- /os -->

<!-- os:mac -->

```sh
shasum -a 256 ./phicomm_a1.ota.bin.raw
awk '$2 == "phicomm_a1.ota.bin.raw" { print }' ./SHA256SUMS
```

<!-- /os -->

<!-- os:linux -->

```sh
sha256sum ./phicomm_a1.ota.bin.raw
awk '$2 == "phicomm_a1.ota.bin.raw" { print }' ./SHA256SUMS
```

<!-- /os -->

Keep power and connectivity stable. Upload once using HTTP/1.0, without a proxy, Expect handshake or automatic retries:

<!-- os:windows -->

```powershell
curl.exe --http1.0 --noproxy "*" --connect-timeout 5 --max-time 300 --header "Content-Type: application/ota-stream" --header "Expect:" --data-binary "@phicomm_a1.ota.bin.raw" --write-out "uploaded=%{size_upload} http=%{http_code}\n" "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
curl --http1.0 --noproxy '*' --connect-timeout 5 --max-time 300 --header 'Content-Type: application/ota-stream' --header 'Expect:' --data-binary '@phicomm_a1.ota.bin.raw' --write-out 'uploaded=%{size_upload} http=%{http_code}\n' "http://${DeviceIp}:8000/OTA"
```

<!-- /os -->

The factory endpoint may close the connection with `(52) Empty reply from server` and `http=000`. Record the uploaded byte count, leave power connected and allow about 2–3 minutes. Check the community hotspot and version in section 5 before taking further action. Do not immediately resend, reset or disconnect power because of this message.

## 4. Method B: DNS redirection for device-initiated download

This path is derived from stock **v1.47** and has not been validated on an A1 device. Use LAN upload first. The original firmware supports these services:

| Purpose | Host / request |
| --- | --- |
| Activation | `phicloudsym.phicomm.com:443/device/active?mac=…&productId=30` |
| Update check | `aircleaner.phicomm.com:443/aircleanerfirmware/checkUpdate` |
| Cloud connection | `aircleanerconnecter.phicomm.com:9000` |

The update response uses top-level `url`, `md5`, `ver` and `enforce` fields. Stock v1.47 accepts a higher numeric version such as `v1.48` with `enforce: true`. This label satisfies the stock version comparison; the installed community version is 0.9.27. The TLS path disables certificate verification but requires a server compatible with the old client.

### Start the temporary Linux service

Download [a1_dns_migration.py](assets/tools/a1-dns-migration/a1_dns_migration.py) and [SHA256SUMS](assets/tools/a1-dns-migration/SHA256SUMS). Install Python 3 and `openssl`. Put them beside the verified A1 raw OTA image, then replace every sample address, MAC and SHA-256:

```sh
sha256sum -c SHA256SUMS
python3 a1_dns_migration.py \
  --image ./phicomm_a1.ota.bin.raw \
  --sha256 REPLACE_WITH_IMAGE_SHA256 \
  --mac 00:11:22:33:44:55 \
  --device-ip 192.168.1.123 \
  --router-ip 192.168.1.1 \
  --server-ip 192.168.1.10
```

Wait for `"kind": "ready"` and keep the terminal open. The service checks the image hash, A1 identity, native header, CRC and MD5 before opening ports. Temporary legacy-compatible HTTPS certificates are stored in `a1-migration-state/`; stop the service after migration and keep it on your LAN.

### Redirect only this A1 on the router

Use an OpenWrt / ImmortalWrt router with `dnsmasq` and `nft`. Reserve A1's DHCP address first. SSH into the router and run these commands in one shell, replacing all addresses. The computer ports 29080, 29443 and 29000 and router port 5533 must be free and reachable.

```sh
RouterIp=192.168.1.1
ServerIp=192.168.1.10
DeviceIp=192.168.1.123
mkdir -p /tmp/a1-dns-migration
cp /etc/hosts /tmp/a1-dns-migration/hosts.before
uci export dhcp > /tmp/a1-dns-migration/dhcp.before
nft list ruleset > /tmp/a1-dns-migration/nft.before

dnsmasq --conf-file=/dev/null --no-hosts \
  --port=5533 --listen-address="$RouterIp" --bind-interfaces \
  --no-resolv --server=127.0.0.1#53 \
  --host-record="aircleaner.phicomm.com,$RouterIp" \
  --host-record="phicloudsym.phicomm.com,$RouterIp" \
  --host-record="aircleanerconnecter.phicomm.com,$RouterIp" \
  --pid-file=/tmp/a1-dns-migration/dns.pid \
  --log-queries --log-facility=/tmp/a1-dns-migration/dns.log

cat > /tmp/a1-dns-migration/rules.nft <<EOF_RULES
table ip a1_dns_migration {
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
nft -c -f /tmp/a1-dns-migration/rules.nft && nft -f /tmp/a1-dns-migration/rules.nft
```

Do not stack duplicate tables. The rules leave global DNS/DHCP configuration unchanged and redirect only the chosen device. The cloud TCP listener records requests without issuing commands.

### Wait for the firmware's update check

DNS changes do not trigger an immediate check. Stock v1.47 registers an hourly timer and tests whether the time string's hour is `18`, together with activation and OTA state. Keep the device online with working time synchronization. Restarting alone does not guarantee an immediate update request.

Watch for `/device/active`, `/aircleanerfirmware/checkUpdate` and `/a1-native.bin`, followed by `download_complete`. Follow step 5 to check the community hotspot and actual version. If no check arrives, use LAN OTA; do not repeatedly restart or send guessed cloud commands.

### Remove temporary services

After transfer/writing has stopped, remove only this table and DNS instance:

```sh
nft delete table ip a1_dns_migration
kill "$(cat /tmp/a1-dns-migration/dns.pid)"
cmp /etc/hosts /tmp/a1-dns-migration/hosts.before
uci export dhcp > /tmp/a1-dns-migration/dhcp.after
cmp /tmp/a1-dns-migration/dhcp.before /tmp/a1-dns-migration/dhcp.after
```

Both `cmp` commands should produce no output. Stop the computer service with Ctrl+C and retain the logs.

## 5. Join the community hotspot and finish setup

First migration may require Wi-Fi setup again. If the old IP is unreachable after upload, scan for the community hotspot.

| Stage | Hotspot | Access while joined |
| --- | --- | --- |
| Community first boot | Open `A1-AP-XXXXXX` | `http://192.168.0.1/`, port 80 |
| Community on the LAN | Find the new IP in the router | `http://device-IP/`, port 80 |

1. Join the `A1-AP-XXXXXX` matching your device's MAC suffix. Stay connected even if your computer reports no Internet.
2. Open `http://192.168.0.1/` and save your 2.4 GHz Wi-Fi settings in the initial setup page. Allow time to connect.
3. Rejoin the router and find A1 by its label MAC. The new IP may match the factory IP or change. Open `http://device-IP/` using port 80, rather than the factory port 8000.
4. Confirm model A1, version **0.9.27**, and working fan, light ring and touch controls.
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

For later community upgrades, use signed packages as described in [firmware updates](firmware-update).
