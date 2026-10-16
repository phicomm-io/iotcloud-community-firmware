# A1 滤芯寿命预估本地验证

实现按风机已施加的非零输出累计使用秒数，默认额定寿命 4320 小时，可设置 1–20000 小时；停止不计时，10% 提醒，耗尽为 0%，更换后确认重置。配置尾部追加 12 字节滤芯记录，v1–v4 配置升级保留既有设置；网络重置保留计时，出厂恢复清零。

## 验证结果

- PASS：`bash tests/run.sh`，完整宿主机回归（含配置迁移、公共命令及文档）。
- PASS：`SANITIZE=1 python3 -m unittest tests.test_a1_filter_storage`，生产存储函数的每小时保存、停止保存、重启保留、写失败重试/回滚、额定寿命校验、重置和 v1–v4 迁移。
- PASS：滤芯计时 C 测试及 ASan/UBSan，覆盖暂停/恢复、毫秒时钟回绕、10% 提醒边界、耗尽及计数饱和。
- PASS：前端单元测试、设备页面语法检查、滤芯页面交互测试和全部四个 demo 的 Chromium 测试。A1 覆盖额定寿命修改、取消/确认重置和移动端无横向溢出。
- PASS：`bash A1/build.sh`，ARM GCC 5.4.1 编译、镜像校验、JSON 浮点支持和按键/定时/MQTT/HTTP 栈预算检查。
- PASS：真实 A1（`10.117.152.111` / `b0f893303134`）标准签名 OTA、重启上线、既有配置保留，及下述滤芯功能验收。
- PASS：隔离 Broker 与生产 MQTT/service worker：五个 HA 实体模板、修改与重置命令、持续运行一分钟主动上报、retained 重放及 HA birth 重新发现。
- PASS：IotCloud Chromium 通过真实 MQTT Broker 发现模拟 A1，显示滤芯状态，保存额定寿命，取消/确认重置并同步状态。修正嵌入页沙箱禁止表单提交的问题，前端 CI 增加平台内表单与重置回归。
- NOT_RUN：真实 A1 的长时间累计、硬断电保存窗口和 Flash 故障。
- NOT_RUN：运行中的 Home Assistant 实例加载及 UI 操作；现有覆盖验证实际发现报文、Jinja 模板和命令闭环。

运行时每累计一小时保存，停止和正常重启/升级预留前也保存；突然断电可能丢失最近不足一小时。Flash 写失败会在状态中报告并重试，此时未保存区间可能更长。PWM 输出是软件施加状态，设备没有转速传感器，本功能不检测风机机械故障或滤芯污染。

## 前端交付状态

前端已通过 PR #2 合入 `phicomm-io/phicomm-community-frontend` 的 main，提交 `264bfcd2810a7597e2f059d7fb8e2a1c39ff2084`。设备 WebUI、平台复用页和 demo 均由这一源码构建，PR 与 main 的前端 CI 均通过。

`frontend.lock.json` 锁定正式 Release `frontend-264bfcd2810a7597e2f059d7fb8e2a1c39ff2084`，SHA-256 为 `ce48d4864aa294794189bd3046056484a3b295f106317f98443ff0344cbde6c2`。已从 GitHub 下载，通过 SHA256SUMS 及 `tools/frontend_release.py` 的归档、清单与各文件校验。其他环境可以直接取得该固定版本。

## 实机刷写与验收

2026-10-16，按用户指定仅更新 `10.117.152.111`，已核对其设备 ID 为 `b0f893303134`。原固件为 `0.9.6-f94b1c`；候选固件为 `0.9.6-f94b1c-dirty`，来自当前未提交功能工作树，并非正式发布。

- 原生 OTA：693628 字节，SHA-256 `ab156c74aa8f41356306a7e2c9072fbafd1a3fce6c109dae2b123f56462fd423`。
- 签名 OTA：693820 字节，SHA-256 `122a7ef6a635e7641080d8cb1dccbe1e2d0146f09da7bf0bb421fbde876299d1`。使用既有可信密钥签名，经公钥验证、候选清单和原生镜像一致性校验后，通过 `/api/v1/ota/upload` 上传；设备返回 `202 OTA verified; rebooting`。
- 新固件上线，设备身份、Wi-Fi、MQTT、面板和 watchdog 状态正常，配置读回保留。
- 风机临时运行于 20% 时，使用量由 0 增至 7 秒；停止后读回保持 7 秒。测试结束恢复原风机关机、设定转速 100% 状态。
- 真实 Chromium 访问设备页面，验证滤芯卡片、额定寿命修改为 6000 小时、取消重置保留 7 秒、确认重置归零，再恢复额定寿命 4320 小时。桌面及 390px 手机布局通过，无页面脚本异常。
- 再次正常重启，滤芯使用量仍为 0 秒、额定寿命仍为 4320 小时；风机、灯光和童锁设置与升级前一致，Wi-Fi、MQTT、面板与 watchdog 正常，配置无待保存或保存错误。

本地证据保存在忽略目录 `build/evidence/a1-filter-deploy/`，含候选清单、升级前后状态、上传响应、计时记录、浏览器验收与截图。截图和配置证据不纳入版本库。

## MQTT 最终实机验收

同日继续在同一台 A1 验证提交 `d8fb96b99a601829d891838eba9a1a0cafeea8c2`，运行版本 `0.9.6-d8fb96`。原生 OTA 为 695932 字节，SHA-256 `3daa1033e3d255daaaab7e9fa9295f4378f69f709f99f5c5be6dcc0ce7700c54`；可信签名包为 696124 字节，SHA-256 `1d7037191eabb9f2dbc28362cab8b2ad305d0ec6253d7d03c44c679040a9f70b`。

- PASS：前一版本直接 HTTP 上传出现 `Upload interrupted`，改由 `ota.start` 下载同一可信签名候选后成功；读回新版本、相同设备身份、既有配置及 M1 绑定。
- PASS：发现实机绑定 M1 时原先会隐藏滤芯 HA 实体，已修正为每台实际 A1 始终保留五个滤芯实体。真实 Broker 收到全部发现配置。
- PASS：通过真实 MQTT 将额定寿命改为 6000 小时并及时收到 retained 状态；20% 风速持续运行一分钟，使用量主动上报至 94 秒；停止后计时暂停。用 HA 重置按钮的实际报文归零，再恢复 4320 小时与原风机、灯光和童锁设置。
- PASS：正式 Release 中的 IotCloud 页面，通过生产 MQTT/WebSocket 控制该实机，验证寿命保存、取消/确认重置、状态同步、操作后控件恢复可用及 390px 手机布局；无页面脚本异常。
- PASS：正常重启后仍为 0 秒 / 4320 小时，配置、绑定和原控制设置保留；MQTT、面板和 watchdog 正常，无待保存配置或保存错误。

前端和平台消费端均已合入各自 main，固定前端 Release 与平台 main 镜像已发布。上述页面验收使用正式前端构建与生产 Broker，未更新线上平台运行实例；运行中的 Home Assistant UI 仍为 NOT_RUN。证据在忽略目录 `build/evidence/a1-filter-mqtt/` 与 `build/evidence/a1-filter-mqtt-pull/`。
