# 斐讯 A1 空气净化器社区固件

适用于原装 EMW3080B 模块，独立应用目录 `A1/`、目标 `A1@MK3080B@moc`。沿用本项目 M1/TC1 的本地 Web、MQTT 和配置方式。控制代码根据 zA1 v0.3.1 固件静态分析重新编写，不依赖斐讯云服务，也没有激活限制。

## 功能

- 风机开关、1～100% 调速，关闭命令为 0%；开启恢复最后一次非零风速。
- 原触摸面板：电源键、相对滑动调速；长按 Wi-Fi 键 2.5 秒开启配网热点。
- RGB 灯环颜色、0～255 亮度、启用与常亮；未常亮时控制后显示两秒。风机运行时为风速条，关闭时显示整圈；Wi-Fi 指示位置显示红/蓝连接状态。
- 童锁仅锁定面板电源和调速，Web/MQTT 仍可控制，Wi-Fi 长按仍可配网。
- 中文离线 WebUI、Wi-Fi 扫描与 AP 配网、MQTT / HA 自动发现、网页管理密码。
- 原生 MiCO OTA 文件上传和 HTTP 直链更新；校验产品、ABI、内核、CRC 与 MD5。
- 恢复最后保存的风机开关、风速、灯环和童锁。首次启动风机关闭，记忆风速为 60%。控制变更在最后一次操作两秒后合并写入 Flash；网络线程每 500ms 检查。重启和 OTA 前同步保存，失败时不继续升级/重启。两秒窗口内断电可能丢失最近操作。

首版不提供旧 zA1 App/UDP 协议、本地定时任务或 M1 联动；这些场景使用 HA 自动化。

## 构建与产物

```sh
./A1/build.sh
```

脚本下载独立 MiCO 3.6.4 SDK，并固定为 `6c465211d3ff8797cd835e400ec54a06530dd476`，应用 `A1/sdk.patch`。补丁关闭通用 LED/按钮及 SDK Wi-Fi 参数变化时的后台配置写入，配置保存统一由 A1 管理。工具为 ARM GCC 5.4.1、Python 2.7.18；下载包检查 SHA256。需要 Linux x86_64、make、C/C++ 主机编译器、git、curl、bzip2、perl、Python 3、zlib 开发包，以及运行旧 GCC 所需的 ncurses/tinfo 库。Ubuntu 22.04 的安装命令见 CI。

默认目录为 `build/a1-sdk/`、`build/a1-tools/`、`build/a1-workspace/`，与 TC1/M1 独立。可用绝对路径环境变量 `A1_SDK_DIR`、`A1_TOOLS_DIR`、`A1_PYTHON2` 指定缓存；SDK 必须是该提交、未应用其他产品补丁的 checkout。工具缓存可以共用，SDK checkout 不可共用。生成的网页头文件在忽略目录 `A1/generated/`。

`build/a1-release/` 包含：

| 文件 | 用途 |
| --- | --- |
| `phicomm_a1.ota.bin` | 本固件的网页升级包 |
| `phicomm_a1.all.bin` | 含 ATE 的完整镜像，供首刷研究使用 |
| `phicomm_a1.usr.bin` | 原生应用镜像 |
| `phicomm_a1.elf` / `.map` | 调试、链接地址和占用分析 |
| `manifest.json` | SDK/补丁摘要、产物 SHA256、镜像信息 |
| `SHA256SUMS` | 五项二进制/调试产物校验和 |

OTA 产品标识为 `PHICOMM_A1_EMW3080`，ABI 3，应用 CPU 地址 `0x08088000`，OTA 中应用偏移 `0x75000`，应用空间 `0x40000`。不能把 M1、TC1、zA1 或 ALL/USR 文件作为本固件的 OTA 包。完整镜像的刷入偏移、分区和设备恢复方式尚需实机确认；构建脚本不执行刷机。

MiCO 3.6.4 的固定内核加填充段与 zA1 v0.3.1 OTA 的前 `0x75000` 字节一致。这证明所用内核/ABI 相容，不代表已经确定 zA1 作者的 SDK 版本。

## 配网与网页

初始名称为 `phicomm_a1_<MAC末六位小写>`，例如 `phicomm_a1_aabbcc`；网页可改名。配网热点为 `A1-AP-<MAC末六位>`，打开 `http://192.168.0.1/`。没有保存 SSID 时立即开启 AP；连接失败约 60 秒后回退 AP。长按 Wi-Fi 开启的 AP 保持到成功保存 Wi-Fi 配置，随后连接成功时关闭。

网页密码默认关闭；设置后用户名 `admin`，密码最多 32 字节。配置接口只返回密码是否已设置，不返回密码。网页和 MQTT 为局域网明文连接，沿用 M1 的实现。

GET `/status`、`/wifi/config`、`/mqtt/config`、`/fan/config`、`/light/config`、`/childLock`、`/ota` 返回状态；`/wifi/scan` 返回扫描结果。POST 使用 JSON：

| 路径 | 示例 |
| --- | --- |
| `/fan/config` | `{"on":true}` 或 `{"speed":75}`；速度 0 关闭，`on` 如同时存在最终优先 |
| `/light/config` | `{"enabled":true,"always_on":false,"brightness":180,"color":{"r":0,"g":0,"b":255}}`；字段可单独修改，颜色三通道须同时提供 |
| `/childLock` | `{"enabled":true}` |
| `/wifi/config` | `{"ssid":"home","password":"12345678","open_network":false}`；同 SSID 空密码保留原密码 |
| `/mqtt/config` | `{"host":"192.168.1.2","port":1883,"user":"","password":"","clear_password":false}`；空 host 停用，空密码保留 |
| `/deviceName` | `{"name":"phicomm_a1_aabbcc"}` |
| `/auth/password` | `{"password":"new-password"}`；空字符串关闭认证 |
| `/wifi/scan` / `/reboot` | `{}` |
| `/ota` | `{"url":"http://192.168.1.2/phicomm_a1.ota.bin"}` |

`/ota/fileUpload` 直接 POST 原始二进制，不使用 multipart。必须有 `Content-Length`；单次上传/下载最长五分钟，每次接收等待最长 15 秒。直链仅支持 HTTP 200、明确长度、无压缩响应；不支持 HTTPS、重定向、chunked。升级期间冻结控制/配置、暂停新的灯环帧，风机保持当前目标；失败解除锁定，成功自动重启并恢复状态。

## MQTT 与 Home Assistant

配置 Broker 后自动发现一台设备，含“空气净化器”、RGB 灯环、童锁、灯环常亮四个实体。HA MQTT 没有独立的 `air_purifier` 类型，主实体采用 `fan`，名称“空气净化器”、图标 `mdi:air-purifier`，提供开关和百分比调速。参见 [MQTT Fan](https://www.home-assistant.io/integrations/fan.mqtt/)、[MQTT Light](https://www.home-assistant.io/integrations/light.mqtt/)、[MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)。

令 `B = phicomm/a1/<完整12位小写MAC>`：

| 命令主题 | 负载 | 状态主题 |
| --- | --- | --- |
| `B/fan/set` | `ON` / `OFF` | `B/fan/state` |
| `B/fan/percentage/set` | 0～100 整数 | `B/fan/percentage/state`（关闭时为 0） |
| `B/light/set` | `{"state":"ON","brightness":180,"color":{"r":0,"g":128,"b":255}}` | `B/light/state`（HA JSON，含 `color_mode:"rgb"`） |
| `B/child_lock/set` | `ON` / `OFF` | `B/child_lock/state` |
| `B/led_always_on/set` | `ON` / `OFF` | `B/led_always_on/state` |

`B/availability` 为 retained `online` / `offline`，异常断线使用遗嘱。状态与发现均 retained，控制命令必须 **不 retained**，固件忽略重放的 retained 控制。灯环关闭/超时后状态为 OFF，颜色和亮度保留；ON、颜色或亮度调整可唤醒灯环。百分比是接受的控制设置，没有实际 RPM 测量。

发现主题为 `homeassistant/<fan|light|switch>/a1_<MAC>/<fan|light|child_lock|led_always_on>/config`。设备 identifier 为 `a1_<MAC>`，entity unique ID 为 `a1_<MAC>_<实体>`，改名不会改变标识。首次连接、重连、改名、收到 `homeassistant/status=online` 时重新发布发现和全部状态。默认 discovery 前缀和 HA birth 主题应保持默认值。固件只用 `B/#` 与 HA birth 两个订阅，避免 SDK 五个消息处理槽限制；精确分发命令，忽略自己的状态回包。

## CI

`.github/workflows/build.yml` 独立构建 TC1、M1 和 A1。A1 缓存 `build/a1-tools`，标签注入固件版本。分支/PR/手动构建保存 artifacts；`v*` 标签在三个构建全部成功后发布六个 OTA/ALL 文件与合并校验和。TC1 的 A1 硬件版本与本目录的 A1 空气净化器为不同产品。

## 证据与实机验收

硬件映射与发送时序见 [PROTOCOL.md](PROTOCOL.md)。目前已完成 ARM 编译/链接、原生 OTA 静态校验、ELF SRAM 代码检查。没有运行单元测试、连接 A1 硬件或执行线上 CI，不声称灯环物理时序、风机、电容触摸及 OTA 启动已实机通过。

实机验收：上电/重启时风机安全电平；风机恢复与 1/60/100% 调速；电源键边沿与相对滑动；童锁及长按 Wi-Fi；灯环顺序、RGB/亮度、两秒熄灭/常亮；错误密码和断网回退 AP；HA 发现、状态同步、遗嘱和 HA 重启；连续控制后 Flash 保存、断电窗口；本地/URL OTA、错误产品/损坏/中断、升级后状态与内存余量。
