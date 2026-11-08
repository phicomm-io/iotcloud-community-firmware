**RC6 为候选测试版本，稳定通道仍为 v0.9.27。升级前备份配置；ESP32 与屏幕 MCU 分别更新。**

**RC6 is a release candidate. The stable channel remains v0.9.27. Back up configuration; update ESP32 and screen MCU separately.**

- **大金 G3 屏幕与传感器支持 / Daikin G3 screen and sensor support**：新增带屏幕 BRY88BBA15G 的 ESP32 固件与独立屏幕 MCU 发布。支持原厂 MCU 协议和当前 RC6 扩展协议、传感器读数、电池状态、RTC 授时、触摸换页、模块启停和启动维护窗口。
  Adds ESP32 firmware and a separate screen MCU release for BRY88BBA15G. Supports the stock MCU and current RC6 extensions, sensor readings, battery status, RTC synchronization, touch page navigation, module control and a startup maintenance window.

- **可编排屏幕、天气与提醒 / Screen composition, weather and reminders**：网页可组合时钟、数值、文字、折线和堆叠图表、天气与提醒，按真实屏幕比例预览。支持六组主题、组件配色、壁纸、语言、亮度和每周调光计划；外部字库失败时降级显示，通信和恢复服务继续工作。新增布局（行 × 列）：天气 1×4、2×4，时钟 1×4，reading 1×2；网页预览与 MCU 同步。
  Compose clocks, readings, text, line and stacked charts, weather and reminders with a preview at the real screen aspect ratio. Includes six themes, per-widget colors, wallpaper, language, brightness and weekly dimming. Font resource failures degrade display while communication and recovery services remain available. New layouts (rows × columns): weather 1×4 and 2×4, clock 1×4, and reading 1×2, with matching WebUI and MCU rendering.

- **ESP32、屏幕和字库在线更新 / Online updates for ESP32, screen and font**：ESP32 可从 GitHub、国内和国际镜像检查并安装签名更新，RC 构建使用独立候选通道。屏幕和字库可在局域网页资源页检查、下载并校验后安装；屏幕大文件上传需要本地直连。免拆迁移先更新 ESP32 并保留原厂屏幕，再按需直接安装社区屏幕应用，保留原厂引导区。安装工具与固件从当前 RC6 下载页配套获取，或使用更新的兼容版本。
  ESP32 can check and install signed updates from GitHub, China and global mirrors; RC builds use a separate candidate channel. Screen applications and fonts can be checked, downloaded and verified from the LAN resource page; large screen uploads require direct LAN access. Wireless migration updates ESP32 first while preserving the stock screen, then optionally installs the community screen application directly while preserving the stock boot region. Get matching tools and firmware from the current RC6 download page, or use a newer compatible release.

- **M1 校时恢复重试 / M1 clock recovery retries**：主控时间读回暂时失败时有界重试，网络校时失败后独立安排重试，减少短暂通信异常造成的时间不同步。
  Retries transient MCU clock readback failures within a bound and schedules network time retries independently, reducing clock desynchronization after temporary communication failures.

- **S30 定时、功率自动化与原厂安装 / S30 schedules, power automation and stock installation**：S30 的插座定时和功率自动化与 TC1 行为对齐，补齐原厂 HTTP 80 端口 OTA 安装工具及教程；继续使用原生 RBL/STOCK_UA 镜像。
  Aligns S30 socket schedules and power automation with TC1 and adds a stock OTA installer using HTTP port 80 with instructions. S30 continues to use native RBL/STOCK_UA images.

- **统一网页与 G3 演示 / Shared WebUI and G3 demos**：更新公共网页的插座自动化和功率刷新，新增 G3 布局、亮度、天气、提醒与在线更新设置，提供屏幕设计和设备网页演示。大金下载分为 1/2 代与带屏幕 3 代。
  Updates shared socket automation and power refresh, adds G3 layout, brightness, weather, reminder and online update settings, and provides screen design and device WebUI demos. Daikin downloads distinguish G1/G2 from the G3 display model.

- **G3 NOR 与壁纸上传修复 / G3 NOR and wallpaper upload fixes**：修复壁纸上传失败，以及上传后 PM2.5/TVOC 读数未恢复、后续 OTA 受阻的问题。NOR 默认使用四线硬件 SFC。
  Fixes wallpaper upload failures and the PM2.5/TVOC readings failing to resume afterward, which also blocked later OTA uploads. NOR defaults to hardware quad SFC.

- **G3 原厂天气与 Linux 迁移 / G3 stock weather and Linux migration**：保留原厂屏幕 MCU 时，ESP32 将真实当前天气转换为原厂温度、湿度和天气图标；室外 PM2.5 保留缺测。新增 Linux 蓝牙配网脚本，免拆迁移教程补齐实际 STA MAC、2.4 GHz Wi-Fi 和原厂网关应答步骤。修复 ESP32 以 80 MHz 运行时 TLS 握手超时导致的天气缺测。
  With the stock screen MCU, ESP32 converts real current weather into factory temperature, humidity and condition fields; outdoor PM2.5 remains missing. Adds Linux Bluetooth provisioning and documents the actual STA MAC, 2.4 GHz Wi-Fi and factory gateway responses for wireless migration. Fixes missing weather caused by TLS handshake timeouts with the ESP32 running at 80 MHz.

- **G3 启动与在线升级负载 / G3 startup and online update load**：ESP32 使用 80 MHz，Wi-Fi 稳定后关闭恢复热点。屏幕先完成字库加载再开启传感器，降低初始化并发负载。修复 OTA 准备失败和上传中断；支持屏幕正常运行时连续更新，完成后恢复传感器。
  ESP32 uses 80 MHz and closes the recovery access point after Wi-Fi stabilizes. The screen loads fonts before enabling sensors to reduce concurrent startup load. Fixes OTA preparation failures and interrupted uploads. Consecutive updates work while the screen runs normally, with sensors resuming afterward.

- **G3 充电与网络图标 / G3 charging and network icons**：电池图标在 USB 供电时显示闪电，联网圆点改为 Wi-Fi 图标，断网时显示斜线。修正外部供电与电池对应的熄屏时间选择，网页预览同步屏幕显示。修复拔掉 USB 后充电图标仍亮的问题。
  The battery icon shows a lightning mark while powered by USB. A Wi-Fi icon replaces the network dot and shows a slash while offline. Corrects the external-power and battery screen-timeout selection and keeps the WebUI preview aligned with the display. Fixes the charging icon remaining on after USB is disconnected.

- **G3 长按关机 / G3 long-press shutdown**：修复长按关机后立即重新启动。关机后等待松键，USB 保持供电时需重新长按约 2 秒开机；短按切页和唤醒保持可用。从熄屏开始持续长按也可关机，不再只执行唤醒。最小维护黑屏状态下也支持长按关机和重新开机；传输固件时禁止按键关机。
  Fixes immediate restart after a long-press shutdown. Shutdown waits for key release; when USB keeps the MCU powered, a new press of about 2 seconds turns it on. Short-press paging and wake remain available. A sustained press from a sleeping display now shuts down instead of only waking it. Long-press shutdown and restart also work in minimal recovery with a black display; power-key shutdown is disabled during firmware transfer.

- **G3 屏幕网页直接安装 / G3 direct screen installation from the WebUI**：首次从原厂屏幕安装和后续社区屏幕升级均可直接选择在线安装或上传文件。一次确认后自动校验、传输并激活，确认新 MCU 正常启动后显示安装成功。修复首次安装后偶发黑屏，以及在线下载误报参数无效的问题。更新提示的换行不再显示为问号。
  Both first installation from stock screen firmware and later community updates support online installation or file upload. One confirmation starts verification, transfer and activation; success appears after the new MCU starts normally. Fixes intermittent black screens after first installation and invalid-parameter errors during online downloads. Update-message line breaks no longer appear as question marks.

- **G3 云端实时读数 / G3 live cloud readings**：同时连接局域网和 MQTT 时，云端卡片和详情页使用较新的传感器读数，避免旧局域网快照使 G3 显示无数据。
  When LAN and MQTT are both connected, cloud cards and details use the newer sensor readings, preventing an older LAN snapshot from leaving G3 with no data.

已知问题：此前在读取多项屏幕配置时观察到过 ESP32 任务看门狗重启，原因仍在排查。
Known issue: An ESP32 task watchdog reset was previously observed while reading multiple screen configuration items. Its cause remains under investigation.

[RC6 下载 / Downloads](https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/v1.0.0-rc.6) · [G3 安装 / G3 installation](https://phicomm-io.github.io/guides/daikin-g3-guide.html)
