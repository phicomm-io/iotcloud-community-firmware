# DAIKIN BRY88AX151K 空气检测仪通用固件（开发版本）

为 BRY88AB151K 原装 ESP32 / 4MB Flash 构建独立固件，复用本项目锁定的离线前端公共组件、设置页及 HTTP 命令封装。PM2005 / PM2105 使用**同一个镜像**，按收到的 I²C 帧格式识别；甲醛模块运行时自动探测，无需构建开关。未验证 BRY88AA151K 的 ADC TVOC 和引脚差异，不能将它作为该型号的已验证固件。

硬件资料参考 [daikin-air-sensor](https://github.com/louliangsheng/daikin-air-sensor/tree/9519f2916563a4f2d1718b2f0db825169192af82)，包括 UART/I²C 引脚和模块协议。这里重新实现驱动与网络服务，不依赖 ESPHome，也不复制上游第三方头文件。

| 功能 | 接线 / 行为 |
| --- | --- |
| PM2005 / PM2105 | I²C SDA23 / SCL22，地址 0x28；按前导字节识别帧格式 |
| HDC1080 | I²C SDA19 / SCL21，地址 0x40 |
| SGP30 | 同温湿度总线，地址 0x58；每秒采集，TVOC 原生单位 ppb |
| CM1106 | UART RX16 / TX17，9600；启动三分钟后允许有效读数 |
| ZE08 甲醛扩展 | UART RX5 / TX18，9600；有效 9 字节应答与校验和证明存在；输出 mg/m³ |
| 指示灯 | CO₂ GPIO4、TVOC/甲醛 GPIO32、PM GPIO33、联网状态 GPIO25；内部 GPIO26（已确认接线，暂未驱动） |
| 配网键 | GPIO34，低电平持续五秒重新打开配网热点，不清空数据；每次按住只触发一次，OTA 写入期间暂停触发 |

探测不依据 GPIO 电平或串口初始化成功。未检出甲醛模块时，HTTP 状态不生成其测量对象，网页隐藏卡片，HA 不发布该实体；曾检出后失联则保留卡片并标记失效。20 秒过期读数返回 `valid:false,value:null`，不会冒充零值。CO₂/甲醛启动期间允许识别模块，但不把预热读数当作测量。未启用自动 CO₂ 校准。

温湿度使用原始测量，未套用参考配置中的经验偏移或倍数；SGP30 的 ppb 不通过经验倍数伪装成质量浓度。PM 协议校验目前基于长度、帧头、完成状态与量程，参考仓库未提供可核验的 I²C 校验和定义，仍需实机核验。

## 构建与安装

安装 PlatformIO Core 后执行：

```sh
./DAIKIN/build.sh
```

产物位于 `build/daikin-release/`：

- `daikin-bry88ab.factory.bin`：首次串口安装的完整镜像，写入 **0x0**。
- `daikin-bry88ab.application.bin`：本固件网页升级使用的应用镜像。
- `manifest.json`、`SHA256SUMS`：源码版本、前端锁、目标与校验信息。

首次安装前完整备份 Flash。进入下载模式后使用 ESPTool，例如：

```sh
python3 -m esptool --chip esp32 --port PORT read_flash 0 0x400000 original.bin
python3 -m esptool --chip esp32 --port PORT write_flash 0 build/daikin-release/daikin-bry88ab.factory.bin
```

松开 BOOT0 后复位。连接 `DAIKIN-<MAC末六位>` 热点，访问 `http://192.168.4.1/`，在网络设置中配置 Wi-Fi。Station 稳定两分钟后关闭临时热点；网络不可用时重新开放恢复入口。网页认证用户名为 `admin`，可设置访问密码。

网页只接受显式确认后的未签名 ESP32 应用镜像，检查 ESP image、应用描述和本产品目标标记，完整写入与校验通过后切换 OTA 槽位。它不支持现有 MiCO 签名包或线上版本下载；不要上传 factory 镜像。尚未实现签名验证、自动故障回滚和掉电实测。

## 接口与联动

`POST /api/v1/command` 使用 `{request_id?,action,params}` / `{request_id?,code,message,data}`，写请求需非空 request_id。支持 status/config/capabilities 查询、设备名称、网页密码、Wi-Fi/MQTT 设置、Wi-Fi 扫描、指示灯、重启与恢复出厂。最近八个写请求在本次启动中去重。串口 115200 也接受每行一个相同 JSON 请求，可用于本地诊断和配网；不会回显密码。

产品管理主题为 `phicomm/v2/<user_key>/daikin/<mac>/...`，使用 TMCP v2，拒绝 retained 命令，QoS1；配置不返回密码。提供 HA 传感器发现。现有云平台和 Broker ACL 必须允许新型号及下述通用传感器主题，不能根据本地固件构建结果推断生产平台已支持。

数据消费者使用 [通用传感器契约](../docs/sensor-provider.md)，A1 不依赖 `daikin` / `m1` 型号。基于 v0.9.21 的 A1 调速算法支持 PM2.5、甲醛、CO₂；通用输入保留 CO₂ 的独立有效性与新鲜度。TVOC 发布为测量值，暂不作为 A1 调速指标。

## 验证

```sh
c++ -Wall -Wextra -Werror -IDAIKIN/include DAIKIN/tests/frames.cpp -o /tmp/daikin-frames
/tmp/daikin-frames
```

`DAIKIN/tests/web.cjs` 对生成 HTML 检查公共前端启动、甲醛缺失/存在/过期和 ESP32 OTA 页面。硬件验证状态另见本次实机记录，主机测试不替代真实模块、闪存掉电和长期运行验证。
