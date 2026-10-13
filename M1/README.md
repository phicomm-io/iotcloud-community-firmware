# Phicomm M1 原模块固件

面向 M1 的 EMW3080B/MK3080B（MX1290）Wi-Fi 模块，保留原 ATSAMD20 传感器与屏幕控制固件。无需斐讯服务端，也不使用 ESP32 转接板。TC1 的 SDK、固件和构建入口保持独立。

## 功能

- 完全本地的中文 WebUI：实时读数、串口诊断、亮度 0–4、Wi-Fi 扫描与配置、MQTT、设备名、时区及网页密码。
- 每 5 秒向 ATSAMD20 查询温度、湿度、甲醛和 PM2.5；15 秒未收到读数即标记失效。
- MQTT 3.1.1、断线重连、遗嘱以及 Home Assistant 自动发现：4 个传感器和亮度 number，HA 重启后重新发布发现配置。
- NTP 校时，通过原串口协议设置屏幕时间；默认 UTC+8。
- 参数持久化；首次启动提供 `M1-AP-<MAC后六位>` 热点，站点连接失败 60 秒后恢复 AP。
- 本地文件上传及直接 HTTP URL 下载 OTA，共用写入与校验路径。

## 构建

Linux x86_64，需安装 git、curl、make、gcc、g++、tar、bzip2、perl、python3、zlib 开发包以及旧 ARM 工具链所需的 libncurses5/libtinfo5（Ubuntu 22.04 可直接安装）。

```sh
./M1/build.sh
```

入口自动下载并固定以下依赖，构建在 `build/m1-workspace`，不使用根目录 TC1 SDK：

| 依赖 | 固定版本 |
| --- | --- |
| MiCO SDK | [3.6.4](https://github.com/MXCHIP/mico-os/tree/6c465211d3ff8797cd835e400ec54a06530dd476)，提交 `6c465211d3ff8797cd835e400ec54a06530dd476` |
| ARM GCC | 5.4.1，5_4-2016q2 |
| Python | 2.7.18，仅用于旧 SDK 构建工具 |
| 目标 | `M1@MK3080B@moc` |

下载的工具链及 Python 源码进行固定 SHA256 校验。可用 `M1_SDK_DIR`、`M1_TOOLS_DIR`、`M1_PYTHON2` 指定本地依赖；SDK 必须为上述提交。`sdk.patch` 只修改独立 SDK 缓存：禁用开发板 LED/按键初始化及 BlueNRG QC 依赖，修复 MQTTYield 错误传播和 OTA 元数据写入返回值。

产物在 `build/m1-release/`：`phicomm_m1.usr.bin`、`phicomm_m1.ota.bin`、`phicomm_m1.all.bin`、`.elf`、`.map`、`manifest.json` 和 `SHA256SUMS`。构建结束验证产品标识、ABI、内核及填充区摘要、应用 CRC16 和 OTA MD5；manifest 记录 SDK 提交和补丁摘要。

## 初次刷入与 OTA

`.ota.bin` 是原生 MiCO MOC OTA 包，含固定内核与应用；本固件的网页升级选择这个文件。`.usr.bin` 仅为应用，不能视为完整首刷镜像。

**本仓库尚未提供经实机验证的首刷流程。** 原厂或 zM1 的升级入口不一定接受新产品标识。初次安装应先备份本机 Flash，确认具体模块、启动器和烧录方式，保留启动器、ATE、参数及校准区；不得把 SDK 自动生成的 `.all.bin` 直接当通用刷机包。按发布要求交付 `phicomm_m1.all.bin`，其中含 ATE；它不代表已经确认安全的通用首刷方案。本构建没有自动烧录操作。应用对应 CPU 地址 `0x08088000`，OTA 内应用偏移 `0x75000`；这些不是任意烧录工具的通用偏移参数。

OTA 写入临时分区后检查容量、应用头、ABI=3、产品 `PHICOMM_M1_EMW3080`、固定内核、CRC16 及整包 MD5，再持久化启动标记并重启。校验失败不设置新启动标记。当前没有签名验证、版本回退限制或应用健康检查回滚。

URL OTA 仅支持直接 `http://`、HTTP 200 和 Content-Length，不支持 HTTPS、重定向、分块或压缩下载；可将文件放在局域网 HTTP 文件服务器。MQTT 也是普通 TCP，网页为 HTTP。网页密码默认关闭，启用后用户名固定 `admin`，密码最长 32 字节。忘记已启用的密码时，恢复热点仍需密码；当前未实现未经确认的物理按键清除功能。

## 使用与 MQTT

连接首次热点后访问 `http://192.168.0.1/`。保存家庭 Wi-Fi 后，获取设备 DHCP 地址访问网页。密码字段留空保留原密码；修改 SSID 时应填写新密码或勾选开放网络。支持的 Wi-Fi 密码为 8–63 字节或开放网络。

配置 MQTT broker 后，HA 启用默认 discovery 即可发现设备。主题中的 ID 为完整小写 MAC（不含冒号）：

| 主题 | 含义 |
| --- | --- |
| `phicomm/m1/<id>/sensor` | 非保留 JSON：temperature、humidity、formaldehyde、pm25 |
| `phicomm/m1/<id>/availability` | 保留 online/offline，断线遗嘱 |
| `phicomm/m1/<id>/sensor_availability` | 读数新鲜度 |
| `phicomm/m1/<id>/brightness/set` | 非保留单字符 0–4 命令 |
| `phicomm/m1/<id>/brightness/state` | 保留的请求亮度，非屏幕 ACK |
| `homeassistant/{sensor,number}/m1_<id>/<field>/config` | 保留的发现配置 |

诊断接口为 `GET /status`。配置 POST 为 `/wifi/config`、`/mqtt/config`、`/deviceName`、`/auth/password`、`/time/config`、`/display/brightness`、`/wifi/scan`、`/reboot`；JSON 字段参见 `web/index.html`。`POST /ota/fileUpload` 接收原始二进制和 Content-Length，`POST /ota` 接收 `{"url":"http://..."}`。

## 硬件与协议依据

依据 [zM1 v0.1.4](https://github.com/a2633063/zM1/releases/tag/v0.1.4) 发布二进制静态分析及上述 SDK pinmap。参考仓库 [pans-phicomm-m1-homeassistant](https://github.com/mocelgoogstnuocca2015-ui/pans-phicomm-m1-homeassistant/tree/222fccc173f49863a8324e1327b3ce2c9b52fcc2) 已克隆到本地 `build/m1-references/pans`；本实现未复制该仓库代码。

| 功能 | SDK 外设 / GPIO |
| --- | --- |
| ATSAMD20 串口 | UART1 TX GPIO9、RX GPIO10，115200、8N1、无流控 |
| 指示灯两路 | PWM4 GPIO13、PWM5 GPIO14，50 kHz |
| 调试串口 | UART2 TX GPIO21、RX GPIO22 |

GPIO13/14 的具体图标、电气极性以及不同批次连线仍需实机确认；不初始化 SDK 开发板 GPIO19 LED、GPIO23 按键。传感器读取和 TM1680 驱动由 ATSAMD20 完成，新 Wi-Fi 固件只操作 UART 和两路 PWM。

下行固定 12 字节，上行固定 20 字节，帧首 `23`、帧尾 `21`；未发现传感器帧校验和，不臆造 CRC。解析器支持分段接收、连续帧及错误帧后重新同步。

- 查询：`23 01 00 00 00 00 00 00 00 00 00 21`。
- 亮度：`23 02 PP ON 00 00 00 00 00 00 00 21`；等级 1–4 的 PP 为 25/50/75/100，ON=1；等级 0 使用最后非零等级的 PP，ON=0。
- 时钟：`23 03 YY YY MM DD hh mm ss 00 00 21`，年份为大端 16 位。
- 读数类型 byte1=01：byte2–3 大端 HCHO 原始值（µg/m³，显示 mg/m³ 时除 1000）；温度 byte5 + byte6/100、湿度 byte7 + byte8/100，本实现取到十分位；PM2.5 为 byte9–10 大端（µg/m³）。其他字段保留未知。
- byte1=0F 且 byte2=0/1 更新屏幕开关状态；其他未确认事件只计数，不清配置或刷写 ATSAMD20。

PWM 占空比采用 zM1 对应的 0.5/3/7/13/20%。联网启用 GPIO14，AP 启用两路，连接过程 GPIO14 闪烁。上述为二进制分析所得配置，实机测量前不声称已确认显示效果。

## 验收状态

本地已完成 ARM 编译、链接及产物静态校验；未运行单元测试，也未连接 M1 硬件。

实机验收依次检查：首刷启动和调试日志；UART 查询/实际传感器读数与原屏幕一致；亮度和物理事件；AP 配网、错误密码和断网恢复；HA 发现、断线遗嘱及传感器失效；NTP/时区；重启后配置；本地及 URL OTA，错误产品、损坏包和中断上传；升级期间运行内存余量。通过这些检查前应视为待实机验证固件。

## 三产品 CI 与 Release

统一工作流 `.github/workflows/build.yml` 独立构建 TC1（A1 硬件版本）、M1 和 A1 空气净化器。分支推送、PR 和手动触发保存构建产物；推送 `v*` 标签且三个构建均成功后，统一发布三个产品各自的 OTA/ALL 镜像及合并 SHA256SUMS。A1 空气净化器文档见 [A1/README.md](../A1/README.md)。
