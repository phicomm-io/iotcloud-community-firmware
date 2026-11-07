# G3 架构、协议与开发资料

G3 使用两套独立工程：ESP32 负责网络、IoTCloud、网页、天气和屏幕 OTA；SWM34SVET6-50 屏幕 MCU 负责传感器、电池充电控制、按键、RTC、LCD 和 LVGL。ESP32 的 GPIO16 TX / GPIO17 RX 以 115200 8N1 与 MCU 通信，保留原厂业务协议，通过能力协商增加扩展。

## 当前开发基线

| 项目 | 资料 |
| --- | --- |
| ESP32 | [RC6 源码](https://github.com/phicomm-io/iotcloud-community-firmware/tree/v1.0.0-rc.6/BRY88BBA15G)、[构建与接线](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/README.md) |
| 屏幕 MCU | [独立仓库 RC6](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/tree/v1.0.0-rc.6)、[构建说明](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/README.md) |
| 扩展服务 | [参数、能力位、二进制布局与 OTA](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/SERVICES.md) |
| 网页与平台 | [统一前端]({{FRONTEND_URL}})、[IoTCloud]({{PLATFORM_URL}})、[通用设备协议](device-api) |
| 使用与资源 | [编排](daikin-g3-layout)、[天气与推送](daikin-g3-services)、[诊断](daikin-g3-diagnostics)、[原厂备份](stock-backups) |

RC6 的当前社区布局格式为 SCN3，9520 字节、288 字节组件记录。未声明扩展能力的原厂 MCU 保留原厂通信路径。串口扩展仍包含会话、序号与 CRC；不要把网页 MessagePack、原厂串口 JSON 与扩展载荷混成一个协议。

## 板级职责与恢复边界

屏幕侧读取温湿度、CO₂、AM1008L 粉尘/TVOC 与可选甲醛模块；ETA6964 参与电池充电管理。PD11 为前侧触摸按键，PD13 为背面网络按钮，PD12 已通过模块启停对照用于 AM1008L SET。P50 是实物确认的 nRESET 测试点；测量和接线仍需核对对应板子。

应用位于 `0x14000`，串口升级保留前 80 KiB 原厂引导区。内部 SRAM 优先保留通信、故障恢复和栈；SDRAM 放帧缓冲、LVGL 与已校验资源；外部 NOR 保存字库/壁纸，失败必须限时降级。原厂引导和应用看门狗的责任划分、传感器补偿的运算顺序都需按资料验证，不凭“应答成功”推断校准已执行。

## 屏幕仓库资料索引

以下链接固定到屏幕 RC6。部分文件记录早期阶段或实验结果，历史布局/资源格式以当前协议和头文件为准。

| 主题 | 文档 |
| --- | --- |
| 实现与阶段记录 | [IMPLEMENTATION](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/IMPLEMENTATION.md)、[2026-11-05 实现](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/IMPLEMENTATION-20261105.md) |
| 串口与能力协商 | [screen-link](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/screen-link.md) |
| 引导、看门狗、恢复 | [BOOTLOADER](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/BOOTLOADER-20261105.md)、[RECOVERY-WDT](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/RECOVERY-WDT.md)、[MCU-RECOVERY](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/MCU-RECOVERY-20261106.md) |
| 验证与首次烧录 | [VALIDATION](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/VALIDATION.md)、[FLASH-TEST](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/FLASH-TEST-20261105.md) |
| 启动与传感器 | [STARTUP-LOADING](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/STARTUP-LOADING.md)、[SENSORS-CALIBRATION](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/SENSORS-CALIBRATION.md)、[MODULE-CONTROL](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/MODULE-CONTROL.md)、[RTC](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/RTC.md) |
| UI 与外观 | [UI-REDESIGN-PLAN](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/UI-REDESIGN-PLAN.md)、[UI-AND-RESOURCES](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/UI-AND-RESOURCES.md)、[APPEARANCE](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/APPEARANCE.md)、[BRIGHTNESS-PLAN](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/BRIGHTNESS-PLAN.md) |
| 字库 | [EXTERNAL-FONT](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/EXTERNAL-FONT.md) |
| 显示性能 | [DISPLAY-DIAGNOSTICS](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/DISPLAY-DIAGNOSTICS.md)、[RENDER-PERFORMANCE](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/RENDER-PERFORMANCE.md)、[PAGE-SWITCH-PERFORMANCE](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/PAGE-SWITCH-PERFORMANCE.md) |

## 构建与验收

分别构建 ESP32 与 ARM 应用，核对应用分区、布局协议快照、恢复服务、资源校验与签名清单。主机测试、CI 构建、下载字节校验和实机运行是不同证据。修改显示驱动后还需测扫描周期、flush、绘制耗时与 OTA 响应；修改电源/传感器控制后需验证冷启动、模块启动、充电和故障维护。发行包与说明从[统一下载区]({{SITE_URL}}#releases)进入。
