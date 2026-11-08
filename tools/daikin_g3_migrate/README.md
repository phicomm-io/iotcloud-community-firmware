# Daikin G3 wireless migration / 大金 G3 免拆迁移

仅用于带屏幕的 **BRY88BBA15G / SENSOR3_ESP32**。先免拆更新 ESP32，保留原厂屏幕；随后可在设备网页直接安装社区屏幕 MCU。

请从[下载页](https://phicomm-io.github.io/#daikin-g3-downloads)获取当前 RC6 的完整 `daikin_G3_migration.tar.gz` 与配套 G3 固件，或使用更新的兼容版本。不要混用不同发布的脚本、ESP32 和屏幕 MCU。校验同一发布的 `SHA256SUMS`。

For **BRY88BBA15G / SENSOR3_ESP32**. Migrate ESP32 first, preserving the stock screen. The device WebUI can then install the community screen MCU directly. Use the complete current RC6 migration bundle and matching G3 firmware from the [download page](https://phicomm-io.github.io/#daikin-g3-downloads), or a newer compatible release. Verify its `SHA256SUMS` and use tools and images from the same release.

The detector must boot and connect to Wi-Fi. Prepare Python 3, OpenSSL, Docker Compose, per-device DNS control, and a Bluetooth-equipped Linux host or Mac. Linux uses BlueZ and `python3-dbus`; macOS also needs Xcode Command Line Tools. The temporary services emulate the factory gateway, MQTT and HTTPS endpoints on your LAN.

## 1. Prepare a temporary LAN service

Use a wired, always-awake service host reachable by the detector. `ADDRESS` below is that host's reachable IPv4 address. Bluetooth provisioning may run on a separate nearby Linux host over SSH. `DEVICE` is the detector's **actual Wi-Fi STA MAC**, obtained from DHCP/the device label, not the virtual gateway MAC or Bluetooth address. Replace the examples before running.

```sh
python3 init.py --address 192.168.1.10 --device AABBCCDDEEFF --output ./lab
# Linux may need sudo for Docker; run init.py as your normal user.
docker compose -f ./lab/compose.json up -d
```

The initial service offers **no update**. Ports LAN TCP 443 and 8883 must be free and reachable by the detector. Keep this temporary anonymous compatibility broker on your trusted LAN; remove it after migration.

## 2. Provision the factory firmware

Configure the three per-device DNS overrides below before provisioning. If already provisioned with a working gateway association, keep it. Otherwise short-press the separate recessed rear network button. Do not long-press. `gateway_mac=020000000001` is a **virtual IP Box**, not the device identity; `gateway_ip` is the service host's reachable address.

### Linux / BlueZ

Use the system Python with `python3-dbus` and a powered BlueZ adapter (Debian/Ubuntu: `sudo apt install bluez python3-dbus`). Scan with `bluetoothctl scan on`, record the selected `SENSOR3_…` name and **BLE address**, then stop that scan with `bluetoothctl scan off`. Copy `wifi.example.json` to a private file, run `chmod 600`, and fill in its fields. Do not derive the Wi-Fi STA MAC from the BLE address.

```sh
cp wifi.example.json /private/path/wifi.json
chmod 600 /private/path/wifi.json
# Edit the private JSON, then run on the Bluetooth host:
/usr/bin/python3 provision_linux.py < /private/path/wifi.json
# Or send it over SSH without storing Wi-Fi credentials on the remote host:
ssh USER@BLUETOOTH_HOST '/usr/bin/python3 /path/provision_linux.py' < /private/path/wifi.json
```

The tool selects the exact name/address, checks the BLUFi response and sends the factory custom message. Check DHCP and gateway association after provisioning. The detector normally closes Bluetooth after Wi-Fi provisioning; short-press the network button again before resending. To send only the gateway MAC, set `"operation": "gateway"` in the private JSON.

### macOS / CoreBluetooth

Copy `wifi.example.plist` to a private file, set permissions `chmod 600`, and fill in the 2.4 GHz SSID/password, selected BLE `device_name` and service host's `gateway_ip`.

```sh
./build_macos.sh
./IotcloudG3Provision.app/Contents/MacOS/provision /absolute/path/to/wifi.plist
```

Allow the app's macOS Bluetooth request. The custom provisioning message includes Wi-Fi plus gateway identity; a generic ESP32 provisioning app is insufficient. To resend only gateway identity, add `<key>operation</key><string>gateway</string>` to the plist. Never commit filled Wi-Fi files.

### Per-device DNS and gateway acknowledgements

For **only this detector**, redirect these three DNS names to the service host and configure its DHCP DNS to use that resolver:

```text
newlifemulti.daikin-china.com.cn
newlifemulti-cdh.daikin-china.com.cn
newlifemulti-cdh-core.daikin-china.com.cn
```

Renew the detector's DHCP lease or restart it normally so it uses the new DNS. Do not redirect other devices or the whole network. Keep the gateway acknowledgements running in another terminal (three-minute default; increase within 3600 seconds if needed):

```sh
python3 local_gateway_responder.py --device AABBCCDDEEFF \
  --gateway 020000000001 --host 192.168.1.10 \
  --empty-gateway --weather-code 1 --seconds 600
```

Run only one responder for this device. The host replies to the selected detector and reports no associated HVAC equipment. On stock MCU `SV.04.00.01.00`, an empty weather response keeps the device retrying weather and prevents it reaching gateway/OTA preparation. `--weather-code 1` supplies a temporary synthetic icon code; temperature, humidity and PM2.5 use missing-value sentinels. Configure a real weather provider in the community WebUI after migration.

Wait for acknowledgements for `snapshot_upload`, `get_weather`, `login_ipbox`, `get_filter_sensor3` and `get_error_sensor3` before triggering OTA. Confirm HTTPS traffic with `docker compose -f ./lab/compose.json logs -f https`.

## 3. Offer and trigger ESP32 only

Download the current release’s **G3 raw ESP32 application** `daikin_BRY88BBA15G.ota.bin.raw` and verify `SHA256SUMS`. Do not use a G1/G2 image, `all.bin`, `.signed`, or the MCU image. `V99.00.00` below is the tested temporary stock OTA target label, used consistently in both commands; after reboot the community firmware reports its own Git-derived version.

```sh
python3 enable.py --directory ./lab --device AABBCCDDEEFF \
  --firmware ./daikin_BRY88BBA15G.ota.bin.raw --version V99.00.00
docker compose -f ./lab/compose.json up -d --no-deps --force-recreate https
python3 trigger.py --device AABBCCDDEEFF --host 192.168.1.10 \
  --version V99.00.00 --trigger-esp32
```

`enable.py` keeps the original broker CA/key. `trigger.py` publishes exactly one non-retained ESP32-only command. Leave power connected during download/reboot. Check the HTTPS log for `artifact-start`/`artifact` (HTTP 200 with the full byte count), then find the detector's DHCP address and open its device page. Verify RC6, actual device identity, Wi-Fi and sensor readings. The factory MCU continues to use its original protocol; no confirmation on the display is normally required. If it does not query/download, inspect gateway association and the three DNS responses instead of repeatedly publishing OTA.

## 4. Finish

Remove the three per-device DNS overrides and restore the original DHCP DNS. Stop the gateway responder and remove the temporary containers with `docker compose -f ./lab/compose.json down`. Remove the private lab directory and Wi-Fi JSON/plist when finished. Keep your factory backups separately.

Community updates: **Settings → Firmware** installs ESP32 updates. For the screen, update the paired ESP32 first, open the device directly on the LAN, then go to **Settings → Screen → Resources and update**. Choose **Check updates → Install screen update**, or select `daikin_G3_screen.bin` from the same release and choose **Upload and install**. Wait for **Screen firmware installed and running normally**, then check its version and sensor readings. The optional font is installed separately. Screen installation preserves the factory boot region and needs no preliminary boot backup or recovery image.

中文步骤：创建临时服务与仅本设备的 DNS → 短按背面网络键，用 Linux 或 macOS 工具配网并配置虚拟网关 → 启动一个带 `--weather-code 1 --empty-gateway` 的应答器，等天气、网关登录、滤网及错误查询得到应答 → 校验当前发布的 G3 raw 应用，启用并发送一次 ESP32 更新 → 核对网页版本与读数 → 撤销 DNS 并停止临时服务。随后可在设备局域网页的“屏幕 → 资源与升级”在线安装或上传同版屏幕应用，等待正常启动。具体设置见[中文指南](https://phicomm-io.github.io/guides/daikin-g3-guide.html)。

## Optional command-line screen installation / 可选命令行屏幕安装

The bundle includes `screen/flash_screen.py` and `screen/backup_screen_boot.py`. Install Python `msgpack` before using them. With the paired ESP32 installed, this command installs the complete application on either stock or community MCU firmware:

工具包中的脚本需要 Python `msgpack`。先更新配套 ESP32，再执行以下命令即可安装屏幕应用：

```sh
python3 -m pip install msgpack
python3 screen/flash_screen.py --url http://DEVICE_IP \
  --image ../daikin_G3_screen.bin --mode install --log install.jsonl
```

If configured, supply the management password through `IOTCLOUD_AUTH=admin:PASSWORD`. Optional backup and recovery details are in the [development guide](https://phicomm-io.github.io/guides/en/daikin-g3-development.html).

设置了管理密码时通过环境变量 `IOTCLOUD_AUTH=admin:密码` 提供。可选引导备份与恢复镜像见[开发资料](https://phicomm-io.github.io/guides/daikin-g3-development.html)。

## Local checks

```sh
python3 -m unittest discover -s . -p 'test_*.py'
```

The tests use loopback HTTPS and temporary fixtures, without changing DNS or contacting a detector.
