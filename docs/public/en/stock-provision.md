# Set up stock A1 / M1 Wi-Fi without the original app

A1 can receive EasyLink packets from a computer. M1 can expose a hotspot and accept Wi-Fi settings over HTTP. These paths do not require the original app or a cloud account.

M1 provisioning has been verified on hardware. Reverse engineering of A1's factory firmware indicates that its provisioning method should also work.

Applies to stock A1 `v1.47`, M1 Wi-Fi module `14.2.4.1` and M1 sensor controller `11.1.2.1`.

<!-- os-picker -->

## 1. Keep an existing connection

If the router already lists your device, keep its settings and obtain its IP address. There is no need to reset it first.

If you changed routers but know the saved SSID, password and security mode, recreating that compatible 2.4 GHz network with DHCP may allow the device to reconnect.

For initial checks, use a separate 2.4 GHz WPA2-Personal/AES network, a short ASCII SSID and an 8–63 character password. Mixed b/g/n mode is suitable; b-only is unnecessary. Avoid WPA3-only. With a shared dual-band SSID, verify the sending computer actually uses 2.4 GHz; temporarily disable 5 GHz if needed. Disable guest/client isolation so the computer can reach the device.

## 2. A1: enable reception, then send from a computer

### Enter provisioning mode

**Hold only the Wi-Fi icon button on A1's panel for 1–2 seconds, then release.** The listening window is about 60 seconds. You can provision again even with saved Wi-Fi credentials.

Start the sender below, then hold the Wi-Fi icon button. No device hotspot connection is needed.

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

Start sending, then immediately **hold only A1's Wi-Fi icon button for 1–2 seconds and release**. The default transmission lasts 50 seconds. Keep the computer on the target Wi-Fi, then check the router's DHCP clients. Add `--port 0` to the launcher command if its port is occupied.

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

The local Python service sends EasyLink wireless provisioning packets; an ordinary online page cannot replace it. The packets carry the Wi-Fi password, so use a trusted wireless environment.

## 3. M1: join its hotspot and submit credentials

### Enter provisioning mode

**Hold M1's body button for about 3–4 seconds, then release** and wait for the open hotspot `EasyLink_XXXXXX`. The suffix is the last six hexadecimal characters of the device MAC.

A short press switches the screen display; holding about 3 seconds starts provisioning. **Holding about 9 seconds and then releasing restores factory network and application settings and reboots.** For ordinary provisioning, release after 3–4 seconds.

An unconfigured device also enters provisioning on boot. Connect and submit credentials within about two minutes of the hotspot appearing. If it is missing or the window expires, hold for 3–4 seconds to retrigger.

M1 also supports EasyLink V2, verified on 2.4 GHz channel 1; channel 6 is not required. Prefer the hotspot HTTP method below.

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
| No M1 hotspot | Hold the body button for 3–4 seconds; refresh the network list and retrigger after the window ends; holding to 9 seconds and releasing restores defaults |
| Blinking Wi-Fi or a changing X icon | Match DHCP and HTTP identity; icons may also reflect factory cloud connectivity |
| M1 hotspot disappears | Rejoin the router and check the lease before assuming failure |
| Lease exists but HTTP fails | Device identity, current IP and client isolation |

Continue with [A1 LAN / public migration](a1-no-disassembly) or [M1 LAN / public migration](m1-no-disassembly). **M1 first migration must start at 0.9.21 or newer.** A1 still requires a physically validated compatible image; M1 results do not establish A1 compatibility.
