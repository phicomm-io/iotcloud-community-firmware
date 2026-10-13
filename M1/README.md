# M1 空气检测仪

## 功能

- 温度、湿度、PM2.5 和甲醛读数；每 5 秒查询，15 秒无新读数标记失效。
- 原屏幕亮度 0–4、NTP 校时与时区设置。
- 中文离线 WebUI、Wi-Fi 扫描与配网、MQTT / Home Assistant 自动发现、网页管理密码。
- 与一台或多台 A1 净化器绑定，配置联动、灯效、定时与睡眠；优先局域网通信，超时回退 MQTT。
- 参数持久化、本地文件与 HTTP 直链 OTA。

适用于原装 EMW3080B / MK3080B（MX1290）Wi-Fi 模块，保留 ATSAMD20 传感器和屏幕控制器，不适用于 ESP32 转接方案。

## 构建方法

Linux x86_64，推荐 Ubuntu 22.04。安装 `build-essential bzip2 curl git perl python3 zlib1g-dev libncurses5 libtinfo5`，在仓库根目录执行：

```sh
./M1/build.sh
```

脚本下载固定提交的 MiCO 3.6.4 SDK、ARM GCC 5.4.1 和 Python 2.7.18，构建 `M1@MK3080B@moc`。SDK 提交为 `6c465211d3ff8797cd835e400ec54a06530dd476`。可用绝对路径 `M1_SDK_DIR`、`M1_TOOLS_DIR`、`M1_PYTHON2` 指定依赖；SDK checkout 不要与 A1 共用。

产物在 `build/m1-release/`：`phicomm_m1.ota.bin`、`phicomm_m1.all.bin`、应用及 ELF/MAP、`manifest.json`、`SHA256SUMS`。网页升级只选 `.ota.bin`，完整镜像不作为通用首刷包。

## 使用方法

首次连接 `M1-AP-<MAC后六位>` 热点，访问 `http://192.168.0.1/` 配置 Wi-Fi。连接失败约 60 秒后恢复热点；联网后通过路由器查询设备 DHCP 地址。

填写 MQTT Broker 后，HA 可自动发现四个传感器和亮度实体。原生主题根为 `phicomm/m1/<12位小写MAC>`：`sensor` 提供非保留 JSON 读数，`brightness/set` 接收非保留单字符 `0`–`4`。网页可设置设备名、屏幕亮度、时间与时区。

「绑定与联动」中填写 A1 的 MAC，可设置局域网 IP、强制 MQTT 或恢复探测。其他配置页操作绑定列表的第一个 A1；组目标使用在线协调者。绑定后 HA 使用逻辑设备，原主题仍可控制和读取。详见 [绑定联动配置](../common/README.md)。

网页密码默认关闭；启用后用户名为 `admin`，密码最多 32 字节。Wi-Fi 和 MQTT 密码留空保留原值。网页和 MQTT 为明文连接，请在可信网络使用。

OTA 选择 `phicomm_m1.ota.bin`；HTTP URL 必须是直接文件地址，不支持 HTTPS、重定向或分块传输。首次安装和恢复限制见 [Wiki：刷机](https://github.com/FUjr/phicomm-community-firmware/wiki/刷机)。
