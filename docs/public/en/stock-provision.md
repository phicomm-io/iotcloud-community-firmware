# Set up stock A1 / M1 Wi-Fi without the original app

A1 can receive EasyLink packets from a computer. M1 can expose a hotspot and accept Wi-Fi settings over HTTP. These paths do not require the original app or a cloud account.

The instructions come from the backed-up factory firmware. The A1 sender passed host tests and emulation of the original ARM decoder; physical A1 provisioning remains untested. M1 hotspot submission returned HTTP 200 in an earlier test. Separately, sending EasyLink V2 from a router's 2.4 GHz radio produced confirmed M1 association, DHCP and factory HTTP identity. Ordinary computer Wi-Fi driver compatibility still needs separate verification.

The analyzed backups are A1 `v1.47` and M1 `14.2.4.1`; other factory versions may differ.

<!-- os-picker -->

## 1. Keep an existing connection

If the router already lists your device, keep its settings and obtain its IP address. There is no need to reset it first.

If you changed routers but know the saved SSID, password and security mode, recreating that compatible 2.4 GHz network with DHCP may allow the device to reconnect.

For initial checks, use a separate 2.4 GHz WPA2-Personal/AES network, a short ASCII SSID and an 8–63 character password. Mixed b/g/n mode is suitable; b-only is unnecessary. Avoid WPA3-only. With a shared dual-band SSID, verify the sending computer actually uses 2.4 GHz; temporarily disable 5 GHz if needed. Disable guest/client isolation so the computer can reach the device.

## 2. A1: enable reception, then send from a computer

### Enter provisioning mode

The factory touch handler starts EasyLink after a particular input remains active for about one second, provided that this press has not triggered it already and no provisioning task is active. The listening window is about 60 seconds. Having saved Wi-Fi credentials does not prevent this trigger.

**The corresponding physical touch icon has not been identified on hardware.** The sampling threshold cannot yet be presented as a verified instruction to hold a named panel button. First boot, Wi-Fi loss, a DNS change or a running sender do not prove that reception has started.

This A1 path listens for wireless packets. Do not expect a hotspot named after the community firmware.

### Use the local web tool

Install Python 3.9 or later. Download the [A1 EasyLink tool bundle](assets/tools/a1-easylink.zip), extract it, and open the `a1-easylink` folder. Keep both Python files and the web asset directory together. No pip packages are needed. The archive is listed in the [SHA-256 checksums](assets/SHA256SUMS).

Connect the computer **over Wi-Fi** to the target 2.4 GHz network. Ethernet cannot replace this connection: A1 needs to receive the computer's wireless frames. Find the IPv4 address of this Wi-Fi adapter, rather than the router, VPN or Ethernet address.

<!-- os:windows -->

In PowerShell, find the WLAN IPv4 address, for example `192.168.1.23`, and start the tool:

```powershell
ipconfig
py -3 --version
py -3 .\a1_easylink_web.py
```

<!-- /os -->

<!-- os:mac -->

In Terminal, identify the Wi-Fi hardware port and read its IPv4 address. Replace `en0` if the actual interface differs:

```sh
networksetup -listallhardwareports
ipconfig getifaddr en0
python3 --version
python3 ./a1_easylink_web.py
```

<!-- /os -->

<!-- os:linux -->

Find the wireless adapter, usually `wlan0` or `wlp…`, then start the tool:

```sh
ip -4 address
python3 --version
python3 ./a1_easylink_web.py
```

<!-- /os -->

The browser opens `http://127.0.0.1:8765`. If it does not, visit the URL printed by the terminal. Enter the same SSID, password and Wi-Fi adapter IPv4.

Once the device's trigger is known, start sending and immediately put A1 into its listening window. The default transmission lasts 50 seconds. Keep the computer on the target Wi-Fi, then check the router's DHCP clients. Add `--port 0` to the launcher command if its port is occupied.

Closing the browser tab does not stop a running transmission. Use the page's stop button or press Ctrl+C in the terminal. “Sending finished” reports the computer's progress, not the device's connection.

### Use the command line instead

<!-- os:windows -->

```powershell
py -3 .\a1_easylink.py --ssid "RescueWiFi" --interface-ip 192.168.1.23
```

<!-- /os -->

<!-- os:mac linux -->

```sh
python3 ./a1_easylink.py --ssid "RescueWiFi" --interface-ip 192.168.1.23
```

<!-- /os -->

The password is requested interactively without echo. Use `--open` explicitly for an open network. `--dry-run` checks parameters and encoding without sending packets.

EasyLink V2 carries two data bytes in multicast destination MAC addresses and a sequence number in packet length. Addresses `239.126.X.Y` map to MAC addresses `01:00:5e:7e:X:Y`. Wi-Fi encryption does not hide that destination header, allowing reception before association. Encoded data also carries the password, so provision on a trusted wireless network. Ordinary UDP broadcast cannot replace this encoding. A normal web page cannot send these UDP packets directly; the local Python helper sends them.

## 3. M1: join its hotspot and submit credentials

### Enter provisioning mode

The factory thread creates an open hotspot named `EasyLink_XXXXXX`, with a MAC suffix and a 120-second window.

A Wi-Fi module GPIO handler starts provisioning on release after a press longer than 50 ms and shorter than 3 seconds. A 3-second hold clears parameters and restarts. The main MCU also has a separate serial reset path.

**The exterior button's connection to that GPIO is not confirmed.** If a clearly marked pairing/reset button exists, a short press and observation of the hotspot can check the entry. Do not guess longer holds if it does not appear. The unconfigured startup branch also starts provisioning; verify that the hotspot actually appears.

EasyLink V2 also works on M1. The successful 2.4 GHz test used channel 1, so channel 6 is not mandatory. V2 depends on the multicast destination MAC and frame length over the air; successful UDP sends alone do not prove device reception. Prefer the hotspot HTTP method below to avoid sender driver and multicast-forwarding differences.

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

## 4. Verify connection and persistence

Obtain the device's new IP from the router and reconnect the computer to the same LAN. Replace this example address:

<!-- os:windows -->

```powershell
$DeviceIp = "192.168.1.123"
curl.exe --http1.0 --noproxy "*" --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

<!-- os:mac linux -->

```sh
DeviceIp="192.168.1.123"
curl --http1.0 --noproxy '*' --connect-timeout 3 --max-time 5 "http://${DeviceIp}:8000/config-read"
```

<!-- /os -->

Check device identity and access, then perform a normal power cycle and verify automatic reconnection. HTTP 200, a disappearing hotspot or completed sending cannot independently prove success.

| Symptom | Check |
| --- | --- |
| A1 never receives | Listening entry, actual 2.4 GHz Wi-Fi transmission, adapter IPv4 |
| A1 does not join | SSID, password, security mode, multicast filtering, VPN and driver behavior |
| No M1 hotspot | Device entry, existing connection, expired window |
| Blinking Wi-Fi or a changing X icon | Match DHCP and HTTP identity; icons may also reflect factory cloud connectivity |
| M1 hotspot disappears | Rejoin the router and check the lease before assuming failure |
| Lease exists but HTTP fails | Device identity, current IP and client isolation |

Continue with [A1 LAN / public migration](a1-no-disassembly) or [M1 LAN / public migration](m1-no-disassembly). **M1 first migration must start at 0.9.21 or newer.** A1 still requires a physically validated compatible image; M1 results do not establish A1 compatibility.

## Appendix: evidence boundary

A1's touch input is `FF 00 nonzero`, with the third byte valid for at least ten samples, one trigger per press and no active task. Its physical icon mapping remains unverified. M1 registers GPIO enum 12; this is not a physical pin number. Its exterior wiring and MCU-controlled button behavior remain unverified. Button entry findings come from the factory binaries, not third-party button instructions. M1 wireless connection was verified under the conditions above; physical A1 reception, exterior button mappings and other Wi-Fi adapters still require separate checks.
