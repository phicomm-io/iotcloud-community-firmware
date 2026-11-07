**RC6 为候选测试版本，稳定通道仍为 v0.9.27。升级前备份配置；ESP32 与屏幕 MCU 分别更新。**

**RC6 is a release candidate. The stable channel remains v0.9.27. Back up configuration; update ESP32 and screen MCU separately.**

- **大金 G3 屏幕与传感器支持 / Daikin G3 screen and sensor support**：新增带屏幕 BRY88BBA15G 的 ESP32 固件与独立屏幕 MCU 发布。支持原厂 MCU 协议和当前 RC6 扩展协议、传感器读数、电池状态、RTC 授时、触摸换页、PD12 模块电源控制及可提前继续的恢复窗口。屏幕应用升级保留原厂引导区。
  Adds ESP32 firmware and a separate screen MCU release for BRY88BBA15G. Supports the stock MCU and current RC6 extensions, sensor readings, battery status, RTC synchronization, touch page navigation, PD12 module power control and an interruptible recovery window. Screen application updates preserve the stock bootloader.

- **可编排屏幕、天气与提醒 / Screen composition, weather and reminders**：网页可组合时钟、数值、文字、折线和堆叠图表、天气与提醒，按真实屏幕比例预览。支持六组主题、组件配色、壁纸、语言、亮度和每周调光计划；外部字库失败时降级显示，通信和恢复服务继续工作。
  Compose clocks, readings, text, line and stacked charts, weather and reminders with a preview at the real screen aspect ratio. Includes six themes, per-widget colors, wallpaper, language, brightness and weekly dimming. Font resource failures degrade display while communication and recovery services remain available.

- **ESP32、屏幕和字库在线更新 / Online updates for ESP32, screen and font**：ESP32 可从 GitHub、国内和国际镜像检查并安装签名更新，RC 构建使用独立候选通道。屏幕和字库可在局域网页资源页检查、下载并校验后安装；屏幕大文件上传需要本地直连。原厂免拆迁移工具先只更新 ESP32，保留原厂 MCU。新脚本与新 ESP32 支持直接安装完整屏幕应用，取消强制引导备份、原厂版本白名单和首装恢复镜像限制；新 MCU 取消引导 MD5 白名单，原厂引导区保留。
  ESP32 can check and install signed updates from GitHub, China and global mirrors; RC builds use a separate candidate channel. Screen applications and fonts can be checked, downloaded and verified from the LAN resource page; large screen uploads require direct LAN access. Wireless stock migration updates only ESP32 first and preserves the stock MCU. The new tool and ESP32 install complete screen applications directly without mandatory boot backup, a stock-version allowlist or recovery-first installation. The new MCU removes its boot MD5 allowlist and preserves the stock bootloader.

- **M1 校时恢复重试 / M1 clock recovery retries**：主控时间读回暂时失败时有界重试，网络校时失败后独立安排重试，减少短暂通信异常造成的时间不同步。
  Retries transient MCU clock readback failures within a bound and schedules network time retries independently, reducing clock desynchronization after temporary communication failures.

- **S30 定时、功率自动化与原厂安装 / S30 schedules, power automation and stock installation**：S30 的插座定时和功率自动化与 TC1 行为对齐，补齐原厂 HTTP 80 端口 OTA 安装工具及教程；继续使用原生 RBL/STOCK_UA 镜像。
  Aligns S30 socket schedules and power automation with TC1 and adds a stock OTA installer using HTTP port 80 with instructions. S30 continues to use native RBL/STOCK_UA images.

- **统一网页与 G3 演示 / Shared WebUI and G3 demos**：更新公共网页的插座自动化和功率刷新，新增 G3 布局、亮度、天气、提醒与在线更新设置，提供屏幕设计和设备网页演示。大金下载分为 1/2 代与带屏幕 3 代。
  Updates shared socket automation and power refresh, adds G3 layout, brightness, weather, reminder and online update settings, and provides screen design and device WebUI demos. Daikin downloads distinguish G1/G2 from the G3 display model.

原厂引导区保持不变，完全断电后可能仍需短触开机。当前版本按现有换页基线发布，性能诊断保留，后续优化另行发布。
The stock bootloader is unchanged; a short power-key touch may still be needed after complete power loss. This release retains the current page-switching baseline and diagnostics; further performance work follows separately.

[RC6 下载 / Downloads](https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/v1.0.0-rc.6) · [G3 安装 / G3 installation](https://phicomm-io.github.io/guides/daikin-g3-guide.html)
