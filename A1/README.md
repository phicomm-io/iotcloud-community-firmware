# A1 空气净化器

## 功能

- 风机开关、1–100% 调速、原触摸面板和童锁；关闭速度为 0。
- RGB 灯环颜色、亮度、常亮及默认灯效；32 颗逻辑灯可分为 1–4 段，显示温度、湿度、空气质量或风量。
- 与一台或多台 M1 绑定；阈值启停、线性与 PID 调速，平均、极值、中位数、去极值平均及顺序故障切换。
- 多净化器组、局域网优先通信、独立超时与 MQTT 回退。
- 每周定时开关、跨午夜睡眠、风速与灯光上限、禁止联动自动启动。
- 中文离线 WebUI、Wi-Fi 配网、MQTT / HA 自动发现、管理密码、配置保存及网页 OTA。

适用于原装 EMW3080B / MK3080B 模块的 **A1 空气净化器**，与 TC1 的 A1 硬件版本无关。

## 构建方法

Linux x86_64，推荐 Ubuntu 22.04。安装 `build-essential bzip2 curl git perl python3 zlib1g-dev libncurses5 libtinfo5`，在仓库根目录执行：

```sh
./A1/build.sh
```

脚本下载固定提交的 MiCO 3.6.4 SDK、ARM GCC 5.4.1 和 Python 2.7.18，构建 `A1@MK3080B@moc`。SDK 提交为 `6c465211d3ff8797cd835e400ec54a06530dd476`。可用绝对路径 `A1_SDK_DIR`、`A1_TOOLS_DIR`、`A1_PYTHON2` 指定依赖；SDK checkout 不要与 M1 共用，工具缓存可以共用。

产物在 `build/a1-release/`：`phicomm_a1.ota.bin`、`phicomm_a1.all.bin`、应用及 ELF/MAP、`manifest.json`、`SHA256SUMS`。网页升级只选 `.ota.bin`，不能混用 TC1、M1 或其他固件的升级包。

## 使用方法

首次连接 `A1-AP-<MAC后六位>` 热点，访问 `http://192.168.0.1/` 配置 Wi-Fi。连接失败约 60 秒后恢复热点；长按 Wi-Fi 键约 2.5 秒可开启热点。联网后通过路由器查询设备 DHCP 地址。

网页提供风机、灯环、童锁与网络设置。首次启动风机关闭，记忆风速为 60%；开启恢复上次非零风速。控制设置在最后一次操作约两秒后保存，立即断电可能丢失最近操作。童锁只限制面板电源和调速。

填写 MQTT Broker 后，HA 可自动发现风机、RGB 灯环、童锁和常亮实体。原生主题根为 `phicomm/a1/<12位小写MAC>`：`fan/set` 接收 `ON/OFF`，`fan/percentage/set` 接收 `0`–`100`，`light/set` 接收 HA JSON 灯命令。控制消息不要 retained。

「绑定与联动」中填写 M1 MAC，等待绑定确认后启用联动。可配置局域网 IP、强制 MQTT、恢复探测、阈值、统计策略、PID 参数、灯带分段和睡眠时段。定时和睡眠依赖有效时钟，默认 UTC+8。睡眠风速上限作用于所有控制来源；禁止自动启动不影响手动或定时开启。详见 [绑定联动配置](../common/README.md)。

网页密码默认关闭；启用后用户名为 `admin`，密码最多 32 字节。网页和 MQTT 为明文连接，请在可信网络使用。

OTA 选择 `phicomm_a1.ota.bin`；HTTP URL 必须是直接文件地址，不支持 HTTPS、重定向或分块传输。首次安装和恢复限制见 [Wiki：A1 刷机](https://github.com/FUjr/phicomm-community-firmware/wiki/A1-刷机)（接线与供电的通用要求见 [刷机前准备](https://github.com/FUjr/phicomm-community-firmware/wiki/刷机前准备)）。
