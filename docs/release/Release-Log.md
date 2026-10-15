# Release Log

发布前在本 Wiki 编辑对应版本章节，再运行快照工具并提交到源码仓库。

## v0.9.5

- 统一三个固件的 WebUI 设计与中英文交互，完善状态、平均读数、定时任务和按键映射。
- 更新 TMCP 通信、绑定与 A1/M1 联动；管理平台复用设备界面，兼容 Legacy 设备名称和计量读数。
- 改进 MQTT 报文处理、回执与连接状态指示，补充主机场景和浏览器回归验证。
- 增加按构建宏配置的国内外 OTA 来源、异步版本检查及签名固件下载入口。
- 建立 Cloudflare R2/Worker 发布源与境内 HTTP 缓存镜像；保留手动文件和 URL 升级。
- 发布说明由 Wiki 维护并在打 tag 前冻结，发布流程校验快照、固件校验和及镜像下载。
- 新增项目介绍、三个固件及管理平台交互演示、最新版下载和历史版本归档。

## v0.9.4

三型号签名 OTA 固件 v0.9.4，基于 main 提交 d50e937；与 v0.9.3 使用相同实现，本次更新版本号并由 CI 重新构建、签名。

- `.ota.bin.signed`：用于 MQTT/URL 和默认 Web 上传，强制验证 P-256/SHA-256 签名。
- `.ota.bin.raw`：用于旧固件首次过渡；新 Web 勾选“允许未签名的固件”即可上传，无二次弹窗。
- `.all.bin`：原生完整烧录镜像。

只有签名、Flash 读回摘要及兼容性检查通过后才启用新镜像。生产私钥位于仓库外，并通过受 v* tag 限制的 GitHub environment 使用。

v0.9.3 已完成 M1 真机 Web/MQTT 拒绝及升级、重启恢复测试。本次 v0.9.4 未刷写设备；TC1/A1 真机、完整资源峰值及断电测试仍待验收。本版允许重装和降级，直链需直接 HTTP 200 + Content-Length。


## v0.9.3

首个签名 OTA 版本，覆盖 TC1 A1、M1、A1。

- `.ota.bin.signed`：P-256/SHA-256 签名包，用于新固件 Web 默认上传及 MQTT/URL 安装。
- `.ota.bin.raw`：原生 OTA。旧固件首次过渡请通过可信本地 Web 上传；新固件勾选“允许未签名的固件”后即可上传，无二次弹窗。
- `.all.bin`：完整原生烧录镜像，不能把签名外壳交给 bootloader。

CI 私钥位于仓库外，并只在受 tag 限制的 GitHub 签名环境中使用；仓库只含公钥。设备擦除前验证头部签名，Flash 读回 SHA-256 和兼容性检查通过后才启用镜像。

M1 真机已验证 raw 过渡、Web raw/signed 上传、MQTT 坏签名/raw 拒绝及 signed 安装和重启恢复。TC1/A1 真机、完整资源峰值及断电/回退仍待验收。本版本允许重装和降级，URL 下载使用直接 HTTP 200 + Content-Length。

设计与验收记录：localdocs/ota-signed-update-v1.md；实现 PR #26。

发布后已下载三型号 Release 产物，逐一验证 P-256 签名、签名摘要及 SHA256SUMS。M1 另通过 MQTT 安装了本 Release 的 CI 签名包，重启后运行 0.9.3-d50e93，MQTT 与传感器数据恢复正常。


## v0.9.2

## M1 实机验证通过的可用版本

本次将 **v0.9.2** 记录为原装 EMW3080B / MK3080B 模块 M1 的实机验证可用基线。源码为 `c1c8f542eb1f61c34fbc12abca5aad681b7ff257`（PR #3），固件显示版本为 `0.9.2-c1c8f5`。

### 本次改动

- WebUI 首页展示实时空气信息及屏幕亮度，二级菜单管理 Wi-Fi、MQTT、设备、绑定联动、升级和诊断；切换页面保留未保存的输入。
- 修正 Wi-Fi 与错误图标的通道映射和熄灭输出：正常连接时 Wi-Fi 常亮、错误标志灭；Wi-Fi 故障时两者同步闪烁；已配置 MQTT 连接异常时 Wi-Fi 常亮、仅错误标志两长两短，未配置 MQTT 不报 MQTT 故障。
- 修复 MQTT 空闲超时误断线及 TCP 分包超过短 yield 期限引发的断线，增加错误计数与最近错误诊断。

### M1 验证记录（2026-10-14）

- 通过设备 Web API 完成 OTA，重启后恢复 Wi-Fi、MQTT 和有效传感器数据。
- 真实设备提供温度、湿度、甲醛、PM2.5 四项读数。
- 与 phicomm-community-mqtt 平台联调：账号主题自动发现、五项 HA 发现配置、网页亮度命令、设备设置值回执、独立 MQTT 客户端恢复亮度均通过；浏览器未出现页面错误。
- 独立 MQTT 客户端连续观察 66 秒，收到 14 次真实传感器上报，无离线事件；Broker 重启后的自动恢复已验证。该观察不代表长期稳定性测试。
- 用户已直接确认：短按屏幕亮灭正常；最终图标映射为 **Wi-Fi 常亮、错误标志灭**。
- 主机测试、WebUI 测试及三款固件 CI 构建通过。

实机验证最初使用同功能源码的开发构建（版本显示 `0.9.1-da45b9-dirty`）；Release 二进制由本标签 CI 构建，下载文件与校验值按本 Release 为准。

### 已知范围

- Wi-Fi/MQTT 故障闪烁时序通过主机测试，实物故障灯效尚未逐项观察验证。
- 实体长按重置、恢复出厂和重置网络尚未实现。
- M1 原屏幕甲醛/PM2.5 彩色指示灯的阈值和手动颜色控制尚无已验证协议，当前不支持。
- 本次实机可用结论只针对 M1；A1 空气净化器及 TC1（A1 硬件版本）附件通过构建，不代表本次完成其硬件验证。

### 下载

M1 网页升级请选择 **phicomm_m1.ota.bin**，不要上传 `.all.bin`；按 `SHA256SUMS` 核对下载文件。升级保留 Wi-Fi、MQTT 与亮度配置。


## v0.9.1

## What's Changed
* treewide: archive TC1 SDK and unify firmware build entry points by @FUjr in https://github.com/FUjr/phicomm-community-firmware/pull/1
* feat: account-scoped MQTT and Git tag firmware versions by @FUjr in https://github.com/FUjr/phicomm-community-firmware/pull/2

## New Contributors
* @FUjr made their first contribution in https://github.com/FUjr/phicomm-community-firmware/pull/1

**Full Changelog**: https://github.com/FUjr/phicomm-community-firmware/compare/0.0.0...v0.9.1

## v0.9.0

**Full Changelog**: https://github.com/FUjr/phicomm-community-firmware/commits/v0.9.0
