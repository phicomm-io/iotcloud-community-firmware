**RC6 为候选测试版本，稳定通道仍为 v0.9.27。升级前备份配置；ESP32 与屏幕 MCU 分别更新。**

**RC6 is a release candidate. The stable channel remains v0.9.27. Back up configuration; update ESP32 and screen MCU separately.**

- **大金 G3 屏幕与传感器支持 / Daikin G3 screen and sensor support**：新增带屏幕 BRY88BBA15G 的 ESP32 固件与独立屏幕 MCU 发布。支持原厂 MCU 协议和当前 RC6 扩展协议、传感器读数、电池状态、RTC 授时、触摸换页、PD12 模块电源控制及可提前继续的恢复窗口。屏幕应用升级保留原厂引导区。
  Adds ESP32 firmware and a separate screen MCU release for BRY88BBA15G. Supports the stock MCU and current RC6 extensions, sensor readings, battery status, RTC synchronization, touch page navigation, PD12 module power control and an interruptible recovery window. Screen application updates preserve the stock bootloader.

- **可编排屏幕、天气与提醒 / Screen composition, weather and reminders**：网页可组合时钟、数值、文字、折线和堆叠图表、天气与提醒，按真实屏幕比例预览。支持六组主题、组件配色、壁纸、语言、亮度和每周调光计划；外部字库失败时降级显示，通信和恢复服务继续工作。新增布局（行 × 列）：天气 1×4、2×4，时钟 1×4，reading 1×2；网页预览与 MCU 同步。
  Compose clocks, readings, text, line and stacked charts, weather and reminders with a preview at the real screen aspect ratio. Includes six themes, per-widget colors, wallpaper, language, brightness and weekly dimming. Font resource failures degrade display while communication and recovery services remain available. New layouts (rows × columns): weather 1×4 and 2×4, clock 1×4, and reading 1×2, with matching WebUI and MCU rendering.

- **ESP32、屏幕和字库在线更新 / Online updates for ESP32, screen and font**：ESP32 可从 GitHub、国内和国际镜像检查并安装签名更新，RC 构建使用独立候选通道。屏幕和字库可在局域网页资源页检查、下载并校验后安装；屏幕大文件上传需要本地直连。原厂免拆迁移工具先只更新 ESP32，保留原厂 MCU。新脚本与新 ESP32 支持直接安装完整屏幕应用，取消强制引导备份、原厂版本白名单和首装恢复镜像限制；新 MCU 取消引导 MD5 白名单，原厂引导区保留。
  ESP32 can check and install signed updates from GitHub, China and global mirrors; RC builds use a separate candidate channel. Screen applications and fonts can be checked, downloaded and verified from the LAN resource page; large screen uploads require direct LAN access. Wireless stock migration updates only ESP32 first and preserves the stock MCU. The new tool and ESP32 install complete screen applications directly without mandatory boot backup, a stock-version allowlist or recovery-first installation. The new MCU removes its boot MD5 allowlist and preserves the stock bootloader.

- **M1 校时恢复重试 / M1 clock recovery retries**：主控时间读回暂时失败时有界重试，网络校时失败后独立安排重试，减少短暂通信异常造成的时间不同步。
  Retries transient MCU clock readback failures within a bound and schedules network time retries independently, reducing clock desynchronization after temporary communication failures.

- **S30 定时、功率自动化与原厂安装 / S30 schedules, power automation and stock installation**：S30 的插座定时和功率自动化与 TC1 行为对齐，补齐原厂 HTTP 80 端口 OTA 安装工具及教程；继续使用原生 RBL/STOCK_UA 镜像。
  Aligns S30 socket schedules and power automation with TC1 and adds a stock OTA installer using HTTP port 80 with instructions. S30 continues to use native RBL/STOCK_UA images.

- **统一网页与 G3 演示 / Shared WebUI and G3 demos**：更新公共网页的插座自动化和功率刷新，新增 G3 布局、亮度、天气、提醒与在线更新设置，提供屏幕设计和设备网页演示。大金下载分为 1/2 代与带屏幕 3 代。
  Updates shared socket automation and power refresh, adds G3 layout, brightness, weather, reminder and online update settings, and provides screen design and device WebUI demos. Daikin downloads distinguish G1/G2 from the G3 display model.

- **G3 NOR 与壁纸上传修复 / G3 NOR and wallpaper upload fixes**：NOR 默认使用四线硬件 SFC，修复壁纸写入与回读；单线 SFC 或 GPIO 仅在诊断启动时由 ESP32 显式选择。修复壁纸上传结束后 PM2.5/TVOC 模块未恢复、读数过期并阻止后续 OTA 的问题。
  NOR uses hardware quad SFC by default, with corrected wallpaper writes and readback. Single SFC or GPIO requires an explicit ESP32 command during diagnostic startup. Fixes the PM2.5/TVOC module remaining disabled after wallpaper uploads, which caused stale readings and blocked subsequent OTA uploads.

- **G3 原厂天气与 Linux 迁移 / G3 stock weather and Linux migration**：保留原厂屏幕 MCU 时，ESP32 将真实当前天气转换为原厂温度、湿度和天气图标；室外 PM2.5 保留缺测。新增 Linux 蓝牙配网脚本，免拆迁移教程补齐实际 STA MAC、2.4 GHz Wi-Fi 和原厂网关应答步骤。修复 ESP32 以 80 MHz 运行时 TLS 握手超时导致的天气缺测。
  With the stock screen MCU, ESP32 converts real current weather into factory temperature, humidity and condition fields; outdoor PM2.5 remains missing. Adds Linux Bluetooth provisioning and documents the actual STA MAC, 2.4 GHz Wi-Fi and factory gateway responses for wireless migration. Fixes missing weather caused by TLS handshake timeouts with the ESP32 running at 80 MHz.

- **G3 启动与在线升级负载 / G3 startup and online update load**：ESP32 使用与原厂一致的 80 MHz，Wi-Fi 稳定后关闭恢复热点。屏幕先完成字库加载再开启传感器；在线 OTA 等待模块断电时保持 MCU 通信，修复升级准备阶段的等待失败。修复 ESP32 写入 Flash 时因周期状态丢失或回执时间差而误中止模块休眠，支持正常屏幕运行时连续 OTA，更新后恢复传感器。
  ESP32 uses the factory 80 MHz limit and closes the recovery access point after Wi-Fi stabilizes. The screen finishes font loading before enabling sensors. Online OTA keeps MCU communication active while waiting for module power-off, fixing preparation failures. Fixes false module-sleep aborts caused by missed periodic status or ACK timing during ESP32 flash writes. Consecutive OTA updates preserve sensor resumption after reboot.

- **G3 充电与网络图标 / G3 charging and network icons**：电池图标在 USB 供电时显示闪电，联网圆点改为 Wi-Fi 图标，断网时显示斜线。修正外部供电与电池对应的熄屏时间选择，网页预览同步屏幕显示。修复拔掉 USB 后充电图标仍亮的问题。
  The battery icon shows a lightning mark while powered by USB. A Wi-Fi icon replaces the network dot and shows a slash while offline. Corrects the external-power and battery screen-timeout selection and keeps the WebUI preview aligned with the display. Fixes the charging icon remaining on after USB is disconnected.

- **G3 长按关机 / G3 long-press shutdown**：修复长按关机后立即重新启动。关机后等待松键，USB 保持供电时需重新长按约 2 秒开机；短按切页和唤醒保持可用。从熄屏开始持续长按也可关机，不再只执行唤醒。最小维护黑屏状态下也支持长按关机和重新开机；传输固件时禁止按键关机。
  Fixes immediate restart after a long-press shutdown. Shutdown waits for key release; when USB keeps the MCU powered, a new press of about 2 seconds turns it on. Short-press paging and wake remain available. A sustained press from a sleeping display now shuts down instead of only waking it. Long-press shutdown and restart also work in minimal recovery with a black display; power-key shutdown is disabled during firmware transfer.

- **G3 屏幕网页直接安装 / G3 direct screen installation from the WebUI**：首次从原厂屏幕安装和后续社区屏幕升级均可直接选择在线安装或上传文件。一次确认后自动校验、传输并激活，确认新 MCU 正常启动后显示安装成功。修复原厂引导交接后偶发的 PLL 未重新锁定及低频启动后的 SDRAM 故障维护；更新提示换行不再显示为问号。
  Both first installation from stock screen firmware and later community updates support online installation or file upload. One confirmation starts verification, transfer and activation; success appears after the new MCU starts normally. Fixes intermittent PLL locking after factory-loader handoff and the resulting low-clock SDRAM recovery. Update-message line breaks no longer appear as question marks.

- **G3 云端实时读数 / G3 live cloud readings**：同时连接局域网和 MQTT 时，云端卡片和详情页使用较新的传感器读数，避免旧局域网快照使 G3 显示无数据。
  When LAN and MQTT are both connected, cloud cards and details use the newer sensor readings, preventing an older LAN snapshot from leaving G3 with no data.

屏幕 OTA 保留原厂引导区。当前版本保留性能诊断，后续优化另行发布。
Screen OTA preserves the factory boot region. Performance diagnostics remain available for further optimization.

[RC6 下载 / Downloads](https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/v1.0.0-rc.6) · [G3 安装 / G3 installation](https://phicomm-io.github.io/guides/daikin-g3-guide.html)
