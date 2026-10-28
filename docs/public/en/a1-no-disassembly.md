# A1 installation compatibility without disassembly

A1 here means the air purifier, not the TC1 A1 hardware revision.

**There is no verified migration package for stock A1 `v1.47`, and no ready-to-use public migration DNS. For first installation, use [A1 / M1 wired installation and recovery](a1-m1-flash). Do not upload an ordinary `.raw`, `.signed` or full Flash image to the factory OTA interface.** Other stock versions also need separate compatibility checks.

Devices already running community firmware should follow [Firmware updates](firmware-update), without the factory migration procedure.

## Connect the stock device

If the router already lists A1, preserve its Wi-Fi configuration and record its IP and MAC. Otherwise use EasyLink as described in [Stock Wi-Fi setup](stock-provision). Connect the computer to the target 2.4 GHz Wi-Fi network. The original app is not required. Start sending in the tool, then hold only the Wi-Fi icon button for 1–2 seconds and release. The receive window lasts about 60 seconds.

## Read-only connection check

These commands only read the factory configuration service. Replace the example IP, then compare the device label and router MAC. Do not publish the full configuration response.

<!-- os-picker -->

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

If there is no response, check the IP, Wi-Fi and router client isolation. An accessible interface establishes network connectivity, not first-migration image compatibility.

## After installation

After wired installation, use [Quick start](quick-start) for Wi-Fi setup. Confirm the model, version, fan and touch controls, then check automatic reconnection and retained settings after a normal power cycle. See [Troubleshooting](faq) if needed.
