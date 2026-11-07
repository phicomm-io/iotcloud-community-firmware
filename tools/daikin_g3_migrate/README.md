# Daikin G3 wireless migration / 大金 G3 免拆迁移

For **BRY88BBA15G / SENSOR3_ESP32**. First migrate only ESP32; the factory screen MCU remains usable. Tools emulate the stock gateway, MQTT and HTTPS services on your LAN. They never flash the MCU or a bootloader. Stock firmware/provisioning differences can prevent the migration; check the device version and readings after reboot.

仅用于带屏幕的三代 BRY88BBA15G。先更新 ESP32，保留原厂屏幕；再按网页的屏幕更新流程迁移 MCU。设备必须正常启动并联网。准备 Python 3、OpenSSL、Docker Compose、可为单台设备指定 DNS 的路由器，以及有蓝牙的 Linux 或 Mac。Linux 使用 BlueZ 和 python3-dbus；macOS 还需 Xcode Command Line Tools。

The republished RC6 archive contains both provisioners. Linux users with an older package can also download [`provision_linux.py`](provision_linux.py) and [`wifi.example.json`](wifi.example.json) from this directory. The instructions below include the stock weather prerequisite discovered during a second-device migration.

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

The tool selects the exact name/address, checks the BLUFi response and sends the factory custom message. A successful send still needs DHCP and gateway verification. The detector normally closes Bluetooth after Wi-Fi provisioning; short-press the network button again before resending. To send only the gateway MAC, set `"operation": "gateway"` in the private JSON.

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

Run only one responder for this device. The host replies to the selected detector and reports no associated HVAC equipment. On stock MCU `SV.04.00.01.00`, an empty weather response keeps the device retrying weather and prevents it reaching gateway/OTA preparation. `--weather-code 1` supplies a temporary synthetic icon code; temperature, humidity and PM2.5 use missing-value sentinels. It is not a weather observation. The migration does not configure a real weather provider.

Wait for acknowledgements for `snapshot_upload`, `get_weather`, `login_ipbox`, `get_filter_sensor3` and `get_error_sensor3` before triggering OTA. Confirm HTTPS traffic with `docker compose -f ./lab/compose.json logs -f https`. Successful MQTT association alone is not an upgrade; an artifact download must follow the next step.

## 3. Offer and trigger ESP32 only

Download the RC6 **G3 raw ESP32 application** `daikin_BRY88BBA15G.ota.bin.raw` and verify `SHA256SUMS`. Do not use a G1/G2 image, `all.bin`, `.signed`, or the MCU image. `V99.00.00` below is the tested temporary stock OTA target label, used consistently in both commands; after reboot the community firmware reports its own Git-derived RC6 identity.

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

Community updates: **Settings → Firmware** checks the online ESP32 source. **Settings → Screen → Resources and update** checks/downloads the paired screen application and optional font, or accepts local files. Screen binary uploads require the browser to be connected directly to the detector on the LAN. With the new paired ESP32 and tool, the complete MCU application can be installed directly on stock firmware; no boot backup, stock-version allowlist or recovery-first step is required. The updater validates NOR staging and leaves the existing boot region intact.

中文步骤摘要：先启动无升级服务并配置仅本机的三个域名 → 短按背面网络键，用 Linux BlueZ 或 macOS 工具配网并补齐虚拟网关 → 启动一个带 `--weather-code 1 --empty-gateway` 的应答器，等待天气、网关登录、滤网和错误查询均得到应答 → 校验 G3 raw 应用，显式启用并发送一次仅 ESP32 升级 → 核对社区网页版本与读数 → 撤销 DNS 和临时服务。天气码仅为通过原厂前置流程的临时占位；温湿度和 PM2.5 是缺测值。配网成功后蓝牙会关闭，需要重发时再次短按网络键。

## Local checks

```sh
python3 -m unittest discover -s . -p 'test_*.py'
```

The tests use loopback HTTPS and temporary fixtures, without changing DNS or contacting a detector.

## Screen MCU / 屏幕 MCU

The ESP32 migration preserves the stock MCU. For an optional screen migration,
the new paired ESP32 and `screen/flash_screen.py --mode install` send the complete
application directly to stock firmware. Backup is optional; the new MCU also
removes the boot MD5 allowlist. The package includes `screen/flash_screen.py` and
`screen/backup_screen_boot.py`; they require Python `msgpack`.

ESP32 迁移保留原厂屏幕。新脚本与配套 ESP32 可直接发送完整 MCU 应用，取消
强制备份、原厂版本白名单和首装恢复镜像限制。新 MCU 也取消本地引导 MD5 白名单。
原厂引导区不改写；串口安装仍依赖它的复制协议，其他引导需核对兼容性。
已安装的旧 MCU 仍执行旧门控，新工具不能取消旧应用内部的检查。

```sh
python3 screen/flash_screen.py --url http://DEVICE_IP \
  --image ../daikin_G3_screen.bin --mode install --log install.jsonl
```

See the [G3 guide](https://phicomm-io.github.io/guides/en/daikin-g3-guide.html) /
[中文教程](https://phicomm-io.github.io/guides/zh/daikin-g3-guide.html).
