# DAIKIN BRY88AX151K 空气检测仪通用固件

为 DAIKIN BRY88AX151K 的 ESP32 / 4MB Flash 板构建通用固件（已测试 BRY88AB151K 接线），复用本项目锁定的离线前端公共组件、设置页及 HTTP 命令封装。PM2005 / PM2105 使用**同一个镜像**，按收到的 I²C 帧格式识别；甲醛模块运行时自动探测，无需构建开关。未验证 BRY88AA151K 的 ADC TVOC 和引脚差异，不能将它作为该型号的已验证固件。

硬件资料参考 [daikin-air-sensor](https://github.com/louliangsheng/daikin-air-sensor/tree/9519f2916563a4f2d1718b2f0db825169192af82)，包括 UART/I²C 引脚和模块协议。这里重新实现驱动与网络服务，不依赖 ESPHome，也不复制上游第三方头文件。

| 功能 | 接线 / 行为 |
| --- | --- |
| PM2005 / PM2105 | I²C SDA23 / SCL22，地址 0x28；按前导字节识别帧格式；同一帧同时读取 PM2.5 / PM10（µg/m³） |
| HDC1080 | I²C SDA19 / SCL21，地址 0x40 |
| SGP30 | 同温湿度总线，地址 0x58；每秒采集，TVOC 原生单位 ppb |
| CM1106 | UART RX16 / TX17，9600；启动三分钟后允许有效读数 |
| ZE08 甲醛扩展 | UART RX5 / TX18，9600；有效 9 字节应答与校验和证明存在；输出 mg/m³ |
| 指示灯 | CO₂ GPIO4、TVOC/甲醛 GPIO32、PM GPIO33、联网状态 GPIO25；内部 GPIO26（已确认接线，暂未驱动） |
| 配网键 | GPIO34，低电平持续五秒重新打开配网热点，不清空数据；每次按住只触发一次，OTA 写入期间暂停触发 |

探测不依据 GPIO 电平或串口初始化成功。未检出甲醛模块时，HTTP 状态不生成其测量对象，网页隐藏卡片，HA 不发布该实体；曾检出后失联则保留卡片并标记失效。20 秒过期读数返回 `valid:false,value:null`，不会冒充零值。CO₂/甲醛启动期间允许识别模块，但不把预热读数当作测量。固件不主动配置模块 ABC 自动校准，不能据此推断模块内部 ABC 已关闭。

## CO₂ 模块诊断与手动校准

通用设置中的“CO₂ 模块诊断与校准”显示模块原始 ppm、状态字节；完整 `status.get` 的 `co2_diagnostics` 另含数据年龄、错误帧数、应答超时数和本次开机的校准状态。当前 DAIKIN 使用旧版 UART 兼容模式 `legacy_reserved`：2019 年 CM1106-C V0.2 将 DF3/DF4 定义为保留字节，因此保留两字节原值，故障标志返回 null、`status_flags_supported:false`，不把状态 1 擅自解释为预热或正常。新版 V0.6 才定义故障位，未确认模块版本前不启用该解释。串口有效应答、数据新鲜度、预热时间和数值范围仍参与测量有效性判断。

校准入口仅在预热三分钟、最近 20 秒内有模块应答后开放（旧版协议不能单凭保留状态字节判断模块故障）。使用可靠参照仪或标准气体确定环境浓度，稳定至少两分钟，填写 400～1500 ppm 整数，勾选环境确认并二次确认。室外不等于固定 400 ppm。参考厂家 [CM1106-C V0.6 说明书第 4.1、4.3 节](https://en.gassensor.com.cn/Product_files/Specifications/CM1106-C%20Single%20Beam%20NDIR%20CO2%20Sensor%20Module%20Specification.pdf)；实际模块子型号的兼容性仍需实机核验。

校准使用现有鉴权和写请求去重机制：

```json
{"request_id":"unique-calibration-id","action":"co2.calibrate","params":{"reference_ppm":430,"reference_stable":true}}
```

`reference_stable:true` 表示操作者确认上述环境条件，固件不能自行证明真实浓度或环境稳定。接口先返回 `accepted` / `queued`，随后从 `status.get` 查询 `co2_diagnostics.calibration.state`：`never`、`queued`、`waiting_ack`、`acknowledged` 或 `timeout_unknown`。只有校验正确的 `16 01 03 E6` 才标记已应答；三秒无应答视为结果未知，不自动重发。每次请求后冷却 60 秒。校准期间清除旧测量，结束后需新测量才能恢复对外读数；已应答不等于精度验证通过，应与参照仪再次比较。校准记录只在 RAM 保存，重启后不保留；模块基线由模块自身管理。

入口前端位于相邻 `phicomm-community-frontend` 仓库。未发布并更新前端锁之前，使用包含该改动的本地前端构建开发包：

```sh
PHICOMM_FRONTEND_SOURCE=/path/to/phicomm-community-frontend ./DAIKIN/build.sh
```

温湿度采用本机原厂固件恢复的补偿：温度减 4.9 °C；湿度先按原始温度换算混合比，经 `q × 0.8915 + 0.2632` 修正，再按补偿后的温度还原相对湿度并限制到 0～100%。湿度随原始温度变化，并非固定加减值。通用设置支持自定义温度减去值（−20～20 °C，默认 4.9）与湿度微调（−30～30 %RH，默认 0），支持恢复默认；设置持久化，保存后清除旧温湿度读数并重新采集。该模型来自原厂程序，不代表经过独立计量校准；SGP30 的 ppb 不通过经验倍数伪装成质量浓度。PM 协议校验目前基于长度、帧头、完成状态与量程，参考仓库未提供可核验的 I²C 校验和定义，仍需实机核验。

## 指示灯规则

在设备设置的“通用设置 → 指示灯规则”（`#settings/device`）中启用指示灯，为 CO₂、TVOC、PM 各设置最多两组规则；PM 灯选择 PM2.5 或 PM10 为数据源，检测到甲醛模块后也可设置甲醛规则。每组包含比较条件（`>`、`≥`、`<`、`≤`）、阈值和常亮 / 常灭 / 快闪 / 慢闪。快闪亮灭各 150 ms，慢闪亮灭各 1 s。规则存入 NVS，重启保留；写入失败保留旧配置。

同一项目多组命中时，高阈值优先。没有命中、没有读数或读数超过 20 秒时熄灯。GPIO33 只根据选定的 PM 数据源判断，另一个 PM 读数不参与亮灯逻辑。TVOC / 甲醛共用 GPIO32，优先显示快闪、其次慢闪、常亮、常灭。未保存自定义规则的旧配置保留原来的 CO₂ >1000、PM2.5 >50、TVOC >300 或甲醛 >0.1 亮灯行为；保存后按全部自定义规则处理。

GPIO25 独立指示系统状态，不受空气质量指示灯总开关影响：未配网三长三短；已配网但 Wi-Fi 未连接快闪；配置了 MQTT 但连接或命令订阅尚未就绪时慢闪；就绪时常亮。未配置 MQTT 不影响就绪判断。节奏及状态优先级复用 `common/indicator.h`。

`config.set` 的 `section` 为 `indicator_rules`，`values` 包含 `pm_source`（`pm25` 或 `pm10`）、三个必需数组 `co2`、`tvoc`、`pm` 和可选数组 `formaldehyde`。数组最多两项，空数组禁用该项目；每项结构为 `{"threshold":1000,"op":"gt","mode":"on"}`，`op` 支持 `gt/gte/lt/lte`，`mode` 支持 `on/off/fast/slow`。阈值必须是有限非负数，上限分别为 10000 ppm、65535 ppb、10000 µg/m³、5 mg/m³，同项不能重复阈值。设置接口、查询和 MQTT 状态使用同一份规则；`indicator_modes` 返回三颗测量灯的当前方式，`system_indicator` 返回系统灯状态。

## 构建与安装

安装 PlatformIO Core 后执行：

```sh
./DAIKIN/build.sh
```

产物位于 `build/daikin-release/`：

- `daikin_BRY88AX151K.all.bin`：首次串口安装的完整镜像，写入 **0x0**。
- `daikin_BRY88AX151K.ota.bin`：未签名应用镜像，仅供显式救援入口。
- 正式发布链生成 `daikin_BRY88AX151K.ota.bin.signed`，日常网页更新使用该签名包。
- `manifest.json`、`SHA256SUMS`：源码版本、前端锁、目标与校验信息。

首次安装前完整备份 Flash。进入下载模式后使用 ESPTool，例如：

```sh
python3 -m esptool --chip esp32 --port PORT read_flash 0 0x400000 original.bin
python3 -m esptool --chip esp32 --port PORT write_flash 0 build/daikin-release/daikin_BRY88AX151K.all.bin
```

松开 BOOT0 后复位。连接 `BRY88AX151K-AP-<MAC末六位大写>` 热点（例如 `BRY88AX151K-AP-AABBCC`），访问 `http://192.168.0.1/`，在网络设置中配置 Wi-Fi。热点 IP 和网关均为 `192.168.0.1`，子网掩码为 `255.255.255.0`；首次启动、按键恢复及断网自动重开均使用此配置。Station 稳定两分钟后关闭临时热点；网络不可用时重新开放恢复入口。网页认证用户名为 `admin`，可设置访问密码。

网页默认接受本项目 PHOTA001 / P-256 签名包，产品编号为 4；验证签名后才开始写入 OTA 槽位，并核对 ESP image、应用描述、目标标记以及 Flash 回读 SHA-256。显式未签名救援入口仍保留。不要上传 factory 镜像或其它产品的签名包。当前不提供在线 URL 下载和精简固件；ESP-IDF 的自动故障回滚与刷写掉电尚未实测。显示型号为 AX，应用描述中的 AB 标记保留以兼容已安装固件的 OTA 迁移。

## 接口与联动

`POST /api/v1/command` 使用 `{request_id?,action,params}` / `{request_id?,code,message,data}`，写请求需非空 request_id。支持 status/config/capabilities 查询、设备名称、网页密码、Wi-Fi/MQTT 设置、Wi-Fi 扫描、指示灯、重启与恢复出厂。最近八个写请求在本次启动中去重。串口 115200 也接受每行一个相同 JSON 请求，可用于本地诊断和配网；不会回显密码。

通过账号内 MQTT 的 `sensor/<mac>/command` 授权、UDP 11288 读取及五秒周期共享 MQTT 测量，为 A1 提供 LAN 优先、MQTT 备用的数据。会话仅在 RAM 保存；授权本身不显示已连接，实际读取后才显示在线状态。

产品管理主题为 `phicomm/v2/<user_key>/daikin/<mac>/...`，使用 TMCP v2，拒绝 retained 命令，QoS1；配置不返回密码。提供 HA 传感器发现。现有云平台和 Broker ACL 必须允许新型号及下述通用传感器主题，不能根据本地固件构建结果推断生产平台已支持。

数据消费者使用 [通用传感器契约](../docs/sensor-provider.md)，A1 不依赖 `daikin` / `m1` 型号。基于 v0.9.23 的 A1 调速算法支持 PM2.5、甲醛、CO₂；每种字段独立聚合；未提供的字段不参与运算或显示对应自动化选项。CO₂ 保留独立有效性与新鲜度。TVOC 发布为测量值，暂不作为 A1 调速指标。

## 验证

```sh
c++ -Wall -Wextra -Werror -IDAIKIN/include DAIKIN/tests/frames.cpp -o /tmp/daikin-frames
/tmp/daikin-frames
c++ -std=c++11 -Wall -Wextra -Werror -IDAIKIN/include DAIKIN/tests/cm1106.cpp -o /tmp/daikin-cm1106
/tmp/daikin-cm1106
c++ -std=c++11 -Wall -Wextra -Werror -IDAIKIN/include -IDAIKIN/.pio/libdeps/bry88ab/ArduinoJson/src DAIKIN/tests/indicators.cpp -o /tmp/daikin-indicators
/tmp/daikin-indicators
```

`DAIKIN/tests/web.cjs` 对生成 HTML 检查公共前端启动、甲醛缺失/存在/过期和 ESP32 OTA 页面。硬件验证状态另见本次实机记录，主机测试不替代真实模块、闪存掉电和长期运行验证。
