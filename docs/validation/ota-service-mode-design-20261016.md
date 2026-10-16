# OTA 模式与 HTTPS 版本查询设计（2026-10-16）

> 本文记录实现前的实验/设计阶段。实际整机实现和设备验证见 [实现报告](ota-tls-implementation-20261016.md)。

本轮完成源码核对、TLS 原型扩展、静态清单生成和本地验证。设备端线程切换尚未接入，未部署、未刷设备。OTA 包始终走现有 signed_ota 验签及产品 finish/激活流程；元数据下载不操作 OTA 分区。

## 建议的职责

保留独立 HTTP、MQTT 连接所有者；一个按需创建的下载工作线程串行负责 check、notes、install，共用 TLS profile 和缓冲。OTA 线程同时接管三条阻塞 I/O 会导致进度查询被 TLS 阻塞，需改造 TLS 非阻塞泵、HTTP 状态机、MQTT 心跳/QoS 和 socket 所有权，工作量及状态远多于早期分支。

OTA 模式时 HTTP/MQTT 在通用 JSON 解析、状态构建、配置快照、发现和业务调度之前走专用分支。仅处理小型 OTA 状态与任务结果；普通命令返回 busy。HTTP 保持已有认证，小型状态直接从固定结构序列化。下载启动先返回 accepted，再异步停联动，避免启动请求等待自身所属线程退出。已开始的上传连接必须保留至 finish/abort，不能在切模式时关闭。

第一版以保留 MQTT OTA 通知设计。MQTT 仅处理 OTA 进度/结果、协议 keepalive/ACK、断线重连；不执行 discovery、完整状态上报、联动订阅检查、普通 service_command 路径。重连可仅订阅 OTA topic。不要删除已有 retained discovery 配置。MQTT 入站限制应在分配大 readbuf 之前生效；payload 分配完成后才忽略，不能限制峰值。

停止联动需请求退出、关闭 UDP、清理 pending RPC/JSON、待发队列和临时发现快照，再由线程自行退出。静态配置、日志、用户配置不随线程退出释放。scheduled action 在维护期间暂停，恢复时避免重放已经执行的任务。保留硬件输出、Wi-Fi、传感器/必要控制及 watchdog。

## 实际能释放什么

|方案|预计回收|限制|
|---|---|---|
|仅 HTTP/MQTT 拒绝业务|临时 JSON、队列、发现等分配|16 KiB MQTT 和 8 KiB HTTP 栈仍存在，不能按栈计入收益|
|联动线程主动退出 + HTTP/MQTT 精简模式|A1/M1 联动栈预算约 12 KiB，另有动态清理|RTOS 回收完成和堆读数需实测确认|
|MQTT 也退出，HTTP 查询进度|另有约 16 KiB MQTT 栈预算|更新期间没有 MQTT 进度，失败恢复后补发结果|
|大线程退出后新建小型协议线程|原栈减去新栈|仍需生命周期、重新连接和小栈水位验证；改一个开关不能回收原栈|

TC1 MQTT 主线程当前栈 8 KiB，另有 4 KiB 接收 worker，结构与 A1/M1 不同，不能套用 28 KiB 回收估计。所有协议缓冲需核对对应 SDK 的 deinit 和 realloc 行为；静态 .bss 不能靠 free 回收。

## 生命周期

IDLE -> RESERVED -> QUIESCING -> TRANSFER -> VERIFY -> REBOOT_PENDING。
任何失败 -> 关闭下载连接并释放 TLS/作业缓冲 -> RESTORING -> IDLE。

RESERVED 在任何线程创建前互斥取得，check/notes/install 不能并发创建 TLS context。QUIESCING 设截止时间，等待工作线程确认退出且 RTOS 已回收；超时取消作业并恢复已退出组件。HTTP/MQTT 进度响应不使用完整 status.get，避免其日志、传感器、配置和订阅快照峰值。

保存本次实际停止的组件掩码、OTA 请求 ID、来源和固定长度结果。恢复按掩码执行且幂等；重建失败保持 RESTORING，定时重试并继续允许 HTTP 进度查询，不能假报恢复成功。恢复后 MQTT 重新订阅、上报状态/发现并补发失败结果。DNS、连接、TLS、HTTP、Flash、验签失败均进入同一清理出口。watchdog 区分线程主动退出和业务超时，不能残留已退出线程的心跳注册。

## HTTPS 版本和 release log

现有 common/ota/update.inc 只支持 HTTP，base+7 解析主机、独立创建 6 KiB ota_check 线程，且 release_log 当前仅返回 URL。不能仅把 base URL 换成 https://。正式接入需由共享下载工作线程替代这段直连 HTTP，并增加异步日志读取/小块读取接口。

最新版本用 /ota/<model>/latest.json，不拉历史总目录。清单字段包括 schema、tag、version、commit、model、签名镜像 path/size/sha256 和 release_log。按 catalog.stable 选择，缺少 signed 下载时不生成，避免旧版 raw 镜像进入自动升级路径。清单上限 4 KiB；下载后可先释放 TLS，再解析 JSON，进一步压低峰值。

日志按版本读取 notes.md，以 1 KiB HTTP Range、512 B sink 回调流式处理；不构建全篇字符串/JSON DOM。设备可暴露带 offset 的小块查询供 UI 拉取，MQTT 只返回版本、状态、路径和短摘要。一次返回全文会重新产生大块内存，需避免。原型总长硬上限 16 KiB，超长明确失败；正式接口如需更长日志应显式分页，不默默截断。

不验证 TLS 证书按用户要求执行。因此版本/说明本身没有可信身份保证，只能作为提示信息；是否写入并启用固件仍由签名包、型号及产品校验决定。

## 本轮证据

使用现有 BearSSL 最小 profile、2048 B TLS 缓冲、1024 B Range。metadata 模式有独立 sink，不调用 signed_ota staging；固件模式保留完整签名验证。

- 真实 Pages /releases/index.json：12470 B，13 次请求，1 条连接，约 1.219 s；通过完整 TLS 流式获取。
- stable 为 v0.9.5；对应 notes.md：776 B，1 次请求，约 0.417 s。
- 现有 Pages /ota/m1/latest.json 实测 HTTP 404；tools/release/archive_pages.py 已加入小清单生成，尚未发布。按实测 catalog 本地生成 TC1 300 B、A1/M1 各 295 B。
- 原型传输测试 8 个方法、18 个场景通过，包含短清单、多块 UTF-8 日志、16 KiB 边界、超限拒绝与原 OTA 验签/损坏/HTTP 错误回归；应用侧 ASan/UBSan 构建同样通过（BearSSL 库沿用未插桩构建）。
- Pages 归档测试 5 项通过，覆盖 stable 选择、缺 signed 不生成、过期入口移除、资产校验和目录穿越拒绝。
- ARM GCC 5.4.1 独立链接 full：text 30566 B、data 32 B、bss 6872 B；相较前轮增加 84 B Flash，固定 RAM 不变（6904 B），下载函数静态栈帧仍为 1264 B。整机网络/线程开销与设备栈水位未在本轮验证。

脱敏原始证据位于 build/tls-probe/results-metadata.json、transport-metadata.log、build-metadata.log；小清单预览位于 build/tls-probe/static-preview/ota/。
