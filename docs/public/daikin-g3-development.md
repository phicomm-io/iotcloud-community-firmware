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

网页使用 MessagePack；原厂串口使用 JSON，扩展帧包含会话、序号与 CRC。未声明扩展能力的原厂 MCU 使用原厂通信路径。

## 板级职责与恢复边界

屏幕侧读取温湿度、CO₂、AM1008L 粉尘/TVOC 与可选甲醛模块；ETA6964 参与电池充电管理。PD11 为前侧触摸按键，PD13 为背面网络按钮，PD12 已通过模块启停对照用于 AM1008L SET。P50 是实物确认的 nRESET 测试点；测量和接线仍需核对对应板子。

应用位于 `0x14000`，串口升级保留前 80 KiB 原厂引导区。内部 SRAM 优先保留通信、故障恢复和栈；SDRAM 放帧缓冲、LVGL 与已校验资源；外部 NOR 保存字库/壁纸，失败必须限时降级。看门狗职责和传感器补偿顺序见下表的对应资料。

## 安装与恢复

首次安装将完整的 421888 字节应用从 ESP32 经原厂 UART 升级协议发送到 MCU。已有社区 MCU 使用 NOR 暂存、回读校验和激活。网页与 `flash_screen.py --mode install` 自动选择路径，安装成功要求新的 MCU 会话、匹配的内部应用 MD5 与正常运行模式。

应用从 `0x14000` 开始，串口升级保留前 80 KiB 原厂引导区。字库、壁纸位于外部 NOR，独立于应用 OTA。可选工具 `backup_screen_boot.py` 对社区 MCU 的引导区双读并比较；已有备份可由安装工具的 `--boot-backup 目录 --reuse-boot-backup` 复用。原厂 MCU 不提供这个读出接口。恢复镜像只启动内部 SRAM 的最小服务，黑屏时仍可通信和升级。

原厂引导交接时保留已启用的 PLL 输出。社区应用在改变分频前先等待晶振稳定、关闭 PLL 与输出，再配置并连续读取两次锁定状态；锁定有时限，失败保留低频 UART 恢复服务。该修复消除了已复现的低频启动后 SDRAM 初始化故障。实现见[时钟初始化](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/55e52f3e922f174b6c515279c8b3d742e42453ae/platform/swm34s/CMakeLists.txt)。

发布站点可压缩 HTTP 传输，浏览器 `fetch` 返回解压后的数据；`Content-Length` 可能为压缩长度。在线安装按实际数据长度校验应用大小，再由 ESP32 校验 SHA-256。实现和压缩/无长度/截断响应检查见[前端修复](https://github.com/phicomm-io/iotcloud-community-frontend/commit/6034bf88ac3be09f3b849ca11f9e7d55fe0335d3)。

### SWD 烧录工具与范围

面向用户的 Windows / Linux / macOS 接线和命令统一放在[刷机教程](daikin-g3-flash#j-link-安装屏幕-mcu)。屏幕芯片为 STAR-MC1；[Synwit 官方 J-Link 插件](https://github.com/Synwit-Co-Ltd/JFlash/blob/7dc3a294fbfc23b7e533006ad301354f597777a6/JLinkDevices.xml)提供 `SWM341xE` 设备，使用 `JLINK_CORE_CORTEX_M33` 和内部 Flash 算法 `SWM341xE.FLM`。不要用不带 Flash 算法的通用内核名称代替设备配置。

RC6 普通应用长度 `0x67000`，写入范围 `[0x14000, 0x7b000)`；前 `0x14000` 引导区、尾部空隙和 `[0x7e000, 0x80000)` 配置区保留。SWD 全量备份应读取两遍 `0x80000` 字节并比较；写后回读应用并核对发布摘要。外部 NOR 不属于本次内部 Flash 烧录范围。

## 电源与 OTA 调度

BRY88BBA15G 原厂 ESP32 `V01.02.00` 使用 ESP-IDF 5.0.1。其电源配置为最高 80 MHz、最低 10 MHz、自动轻睡眠；`sensor3` 与 `uart_resp` 使用 APB 频率锁协调通信。配置数据位于 `0x3f4060d8`，由 `0x400d8e04` 传给 `esp_pm_configure`，原厂启动日志确认 80 MHz。

原厂 OTA 经 HTTPS 下载并通过 UART 与 MCU 协调。下载块长为 `0x800`，每轮 `vTaskDelay(2)`；`esp_ota_begin(next_partition, -1, ...)` 先擦除备用分区，下载后校验 MD5、结束 OTA 并切换启动分区。G3 使用这条路径，G1/G2 的 TCP 8555 工具不适用。

社区 G3 保留 Arduino 2.0.17 API，以 ESP-IDF 4.4.7 组件方式从源码构建 SDK，启用 `CONFIG_PM_ENABLE` 与 `CONFIG_FREERTOS_USE_TICKLESS_IDLE`，使用原厂的 10–80 MHz 动态调频和自动轻睡眠。G1/G2 的 SensorKit2 原厂启动代码选择 240 MHz，社区保持 240 MHz 及预编译 Arduino SDK。两种 ESP32 的 Wi-Fi 均使用 `WIFI_PS_MIN_MODEM`。

G3 实机复现了轻睡眠唤醒后的 `vTaskStepTick` 断言。构建在临时目录生成 PM 源码副本，移植 [Espressif 的有界 tick 补偿修复](https://github.com/espressif/esp-idf/commit/5a5ef9fb08aa48464487ddfe7d9e9fe4127f246d)：仅对不超过两个 tick 的超时限制补偿值，更大的超时仍触发原断言。构建先校验 SDK 源码摘要；SDK 升级时必须重新核对。该处理可能使 RTOS tick 略落后于实际时间，`esp_timer` 毫秒时基和屏幕时钟继续按实际时间运行。

G3 GPIO2 是屏幕 MCU 的唤醒输入。电平为高或正在升级时持有 APB 锁；UART 发送结束并经过至少 50 ms 通信尾期后释放发送锁。屏幕 UART 和调试 UART 使用 `REF_TICK`，保持降频后的波特率。主循环允许 10 ms 空闲，满足 tickless idle 的入睡条件。`diagnostics.get.power_management` 返回即时 PM 配置、唤醒输入、两把锁及最近唤醒原因；`status.get` 包含其缓存快照。实现依据见 [ESP-IDF 电源管理](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/system/power_management.html)。

屏幕 MCU 原厂 `SV.04.00.04.00` 在 `0x4ef80` 执行 `WFI`，通信任务在约一秒后清低 PA5 唤醒输出。社区 MCU 在无串口待处理数据时等待中断，保留显示、SDRAM 和外设时钟；发送前先置高 PA5，结束一秒且 UART 空闲后清低。周期状态改为两秒，按键和命令仍即时响应。此次未调整 MCU 主频，也未移植原厂 FreeRTOS 的 tickless 时间补偿。反汇编依据与实现见屏幕工程的 `docs/POWER-MANAGEMENT.md`。

上传每次至多写入 512 B，在线下载缓冲为 1024 B，每 16 KiB 主动让出执行，并随接收逐扇区擦除。启用休眠的 SDK 构建仍保留原有 OTA 目标标识、应用描述和原厂分区；整机电流需另行实测。

屏幕启动先完成字库加载与校验，再开启传感器；三个传感器 UART 保留独立中断缓冲。升级时降低亮度并将 PD12 SET 拉低。ESP32 通过有界租期及匹配回执续约模块休眠，保持 UART owner 接收。Flash 写入期间可能丢失周期状态，因此已确认租期不依赖每次周期通告；回执年龄使用接收后的当前时间计算，避免旧循环时间戳产生无符号下溢。实现见[模块控制](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/screen_module.inc)。

ESP32 单独重启后会请求屏幕重新协商扩展能力。如果启动请求丢失，收到带有效 CRC 的旧会话扩展帧时再次请求重置，恢复能力协商、布局和显示策略同步；无需重启 MCU。实现见[串口桥接](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/bridge.c)。

## 内存与资源

SCN3 布局为 9520 字节、每组件 288 字节。串口缓存优先放入按 32 位访问的 IRAM，通过对齐字读写；分配失败时使用 DRAM。`screen.status` 的 `layout_cache_bytes` 和 `layout_cache_storage` 报告长度及 `iram` / `dram` / `unallocated`。状态编码临时区和布局上传缓冲按需申请，完成或超时后释放。

TLS 使用可逐字节访问的内部 DRAM。Open-Meteo 准入需要 72 KiB 字节堆及至少 24 KiB 连续块，其他提供商需要 84 KiB 字节堆。`screen.weather.get` 报告字节堆、最大连续块、worker 前后水位、尝试次数及退避状态。原厂通路标记 `delivery:factory`，`factory` 保存最近写入 UART 的温度、湿度、天气码和室外 PM2.5 字段，`factory_sent_age_ms` 为发送年龄。

NOR 默认四线硬件 SFC，用于字库、壁纸与升级暂存的读写和回读。单线 SFC 或 GPIO 只由 ESP32 在诊断启动窗口显式选择，不自动切换。字库分批读取时校验，完成后缓存到 SDRAM；错误与超时使用内置字体和纯色背景。总线、资源加载和初始化顺序见[板级实现](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/55e52f3e922f174b6c515279c8b3d742e42453ae/platform/swm34s/board.c)与[启动实现](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/55e52f3e922f174b6c515279c8b3d742e42453ae/platform/swm34s/application.c)。

## 显示测量

每次采集最多 96 条记录，每批读取 8 条。整份报告使用同一个 generation，并记录丢弃计数。扫描周期来自 LCD 帧结束中断，包含中断延迟；flush 耗时包含像素拷贝；monitor 记录 LVGL 每轮刷新；相邻 flush 间隔包含绘制和调度。

当前驱动使用单份扫描帧缓冲和 40 行绘制缓冲，完整页面通常分 22 次拷贝。原厂驱动使用两份完整缓冲，在帧边界装载显示地址。现有诊断不修改扫描时钟或缓冲策略；布局和显示驱动的性能记录见下表中的显示性能文档。

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

分别构建 ESP32 与 ARM 应用，核对应用分区、布局协议快照、恢复服务、资源校验与签名清单。记录源码版本、固件摘要、设备安装结果及重启后的运行状态。修改显示驱动后还需测扫描周期、flush、绘制耗时与 OTA 响应；修改电源/传感器控制后需验证冷启动、模块启动、充电和故障维护。发行包与说明从[统一下载区]({{SITE_URL}}#releases)进入。
