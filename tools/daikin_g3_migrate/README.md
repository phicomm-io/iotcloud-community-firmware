# Daikin G3 wireless migration / 大金 G3 免拆迁移

For **BRY88BBA15G / SENSOR3_ESP32**. First migrate only ESP32; the factory screen MCU remains usable. Tools emulate the stock gateway, MQTT and HTTPS services on your LAN. They never flash the MCU or a bootloader. Stock firmware/provisioning differences can prevent the migration; check the device version and readings after reboot.

仅用于带屏幕的三代 BRY88BBA15G。先更新 ESP32，保留原厂屏幕；再按网页的屏幕更新流程迁移 MCU。设备必须正常启动并联网。准备 Python 3、OpenSSL、Docker Compose、可为单台设备指定 DNS 的路由器；macOS 蓝牙配网还需 Xcode Command Line Tools。

## 1. Prepare a temporary LAN service

Use a wired, always-awake host in the detector's LAN. `ADDRESS` below is that host's LAN IPv4 address. `DEVICE` is the detector's **actual Wi-Fi STA MAC**, obtained from DHCP/the device label, not the virtual gateway MAC or Bluetooth address. Replace the examples before running.

```sh
python3 init.py --address 192.168.1.10 --device AABBCCDDEEFF --output ./lab
# Linux may need sudo for Docker; run init.py as your normal user.
docker compose -f ./lab/compose.json up -d
```

The initial service offers **no update**. Ports LAN TCP 443 and 8883 must be free and reachable by the detector. Keep this temporary anonymous compatibility broker on your trusted LAN; remove it after migration.

## 2. Provision the factory firmware

If already provisioned with a working gateway association, keep it. Otherwise short-press the separate recessed rear network button. Do not long-press. On a Mac, copy `wifi.example.plist` to a private file, set permissions `chmod 600`, and fill in the 2.4 GHz SSID/password, selected BLE `device_name` and the service host's `gateway_ip`. `gateway_mac=020000000001` is a **virtual IP Box**, not the device identity.

```sh
./build_macos.sh
./IotcloudG3Provision.app/Contents/MacOS/provision /absolute/path/to/wifi.plist
```

Allow the app's macOS Bluetooth request. The custom provisioning message includes Wi-Fi plus gateway identity; a generic ESP32 provisioning app is insufficient. To resend only gateway identity, add `<key>operation</key><string>gateway</string>` to the plist. Never commit the filled plist.

For **only this detector**, redirect these three DNS names to the service host and configure its DHCP DNS to use that resolver:

```text
newlifemulti.daikin-china.com.cn
newlifemulti-cdh.daikin-china.com.cn
newlifemulti-cdh-core.daikin-china.com.cn
```

Renew the detector's DHCP lease or restart it normally so it uses the new DNS. Do not redirect other devices or the whole network. Keep the gateway acknowledgements running in another terminal (three-minute default; increase within 3600 seconds if needed):

```sh
python3 local_gateway_responder.py --device AABBCCDDEEFF \
  --gateway 020000000001 --host 192.168.1.10 --empty-gateway --seconds 600
```

The host replies only to the selected detector's stock association/snapshot requests and reports no HVAC equipment. Confirm traffic in `docker compose -f ./lab/compose.json logs -f https`. Successful MQTT association alone is not an upgrade; an artifact download must follow the next step.

## 3. Offer and trigger ESP32 only

Download the RC6 **G3 raw ESP32 application** `daikin_BRY88BBA15G.ota.bin.raw` and verify `SHA256SUMS`. Do not use a G1/G2 image, `all.bin`, `.signed`, or the MCU image. `V999` below is the temporary stock OTA target label, used consistently in both commands; after reboot the community firmware reports its own Git-derived RC6 identity.

```sh
python3 enable.py --directory ./lab --device AABBCCDDEEFF \
  --firmware ./daikin_BRY88BBA15G.ota.bin.raw --version V999
docker compose -f ./lab/compose.json up -d --no-deps --force-recreate https
python3 trigger.py --device AABBCCDDEEFF --host 192.168.1.10 \
  --version V999 --trigger-esp32
```

`enable.py` keeps the original broker CA/key. `trigger.py` publishes exactly one non-retained ESP32-only command. Leave power connected during download/reboot. Check the HTTPS log for `artifact-start`/`artifact-complete`, then find the detector's DHCP address and open its device page. Verify RC6, actual device identity, Wi-Fi and sensor readings. The factory MCU continues to use its original protocol; no confirmation on the display is normally required. If it does not query/download, inspect gateway association and the three DNS responses instead of repeatedly publishing OTA.

## 4. Finish

Remove the three per-device DNS overrides and restore the original DHCP DNS. Stop and remove the temporary containers with `docker compose -f ./lab/compose.json down`. Remove the private lab directory and Wi-Fi plist when finished. Keep your factory backups separately.

Community updates: **Settings → Firmware** checks the online ESP32 source. **Settings → Screen → Resources and update** checks/downloads the paired screen application and optional font, or accepts local files. Screen binary uploads require the browser to be connected directly to the detector on the LAN. Before the first stock-to-community MCU update, retain the 80 KiB boot backup and satisfy the explicit factory bootstrap gate described in the G3 guide. The updater validates NOR staging and leaves the existing boot region intact.

中文步骤摘要：先启动无升级服务 → 短按背面网络键，用原生蓝牙工具配网并补齐虚拟网关 → 仅重定向本机三个域名并更新 DHCP DNS → 启动网关应答 → 校验 G3 raw 应用，显式启用并发送一次仅 ESP32 升级 → 核对社区网页版本与读数 → 撤销 DNS 和临时服务。不要使用 G1/G2 镜像、整片镜像或屏幕应用执行第一阶段迁移。

## Local checks

```sh
python3 -m unittest discover -s . -p 'test_*.py'
```

The tests use loopback HTTPS and temporary fixtures, without changing DNS or contacting a detector.

## Screen MCU / 屏幕 MCU

The ESP32 migration preserves the stock MCU. For an optional screen migration,
use the separate recovery image, back up the first 80 KiB twice, then install
the full application. The package includes `screen/flash_screen.py` and
`screen/backup_screen_boot.py`; they require Python `msgpack`.
Follow the [G3 guide](https://phicomm-io.github.io/en/daikin-g3-guide/).

ESP32 迁移保留原厂屏幕。若要更换屏幕固件，先安装独立恢复镜像、双读备份前
80 KiB 引导区，再安装完整应用。包内附 `screen/flash_screen.py` 和
`screen/backup_screen_boot.py`，依赖 Python `msgpack`。
按 [G3 教程](https://phicomm-io.github.io/daikin-g3-guide/)执行。
