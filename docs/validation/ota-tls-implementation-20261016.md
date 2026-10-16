# github.io TLS OTA 实现与设备验证（2026-10-16）

本报告记录工作树中的实际实现；之前三份 feasibility/resource/design 文档保留为历史实验记录。
验证仅使用 A1 `10.117.152.111` / `b0f893303134`、M1 `.110` / `b0f8932864b6`、TC1 `.102` / `D0BAE463E12C`。
当前构建版本均为 `0.9.5-d4911b-dirty`，不能只凭版本字符串区分候选镜像。镜像散列、重启前后 uptime、配置和运行状态分别留证。

## 实现

- `common/ota/tls_profile.*`：BearSSL 0.6，TLS 1.2，单一 `TLS_RSA_WITH_AES_128_GCM_SHA256`，RSA i32、小型 AES/GCM、SHA-256、SNI；叶证书仅解码 RSA 公钥，不校验证书链、域名、有效期或证书签名。
- **固件包的 ECDSA P-256 签名、型号、Flash 读回 SHA-256，以及原生 ABI/产品/kernel/CRC/MD5 校验保留。** TLS 证书策略没有改变固件包验签要求。
- `common/ota/fetch.*`：设备和主机测试编译同一传输实现。HTTPS 使用 1024 B Range、严格检查 206/Content-Range/Content-Length，复用连接；HTTP 镜像保持 200 流式下载。每次写入最多 512 B，不缓存整包。
- `common/ota/mode.*`：检查版本、说明、更新共用互斥作业。A1/M1 先停止联动 worker，清理 UDP、队列和动态快照并释放线程；失败先释放 TLS，再重建 worker，内存不足时保持 restoring 并重试。
- MQTT/HTTP 保留原线程，用 OTA 分支提供小型状态、取消和作业结果；其他业务请求返回 busy。**没有回收 MQTT 线程栈**，也没有将三个 socket 强行交给同一阻塞线程。
- 日志环形缓存改为字典 ID + int32 参数；读取时恢复原事件文本。MQTT 阶段错误码保留。新增事件必须补充字典，未知词汇不记录。
- 元数据限制：latest.json 4096 B、说明 16384 B；说明每次返回最多 1024 B，并处理 UTF-8 分页边界。TLS 先释放，再解析版本 JSON。
- DNS/连接/I/O 有 10 s 等待上限；socket 保持非阻塞，网络等待每 200 ms 检查取消；传输预算 180 s（临界时开始的新连接最多另占 10 s），校验、恢复和重启另计。不是所有任务都能在 200 ms 内结束：CPU 验签、Flash 操作和调度仍占时间。提交启动分区后拒绝取消。
- Flash 擦除改为 4 KiB 分段并检查取消，段间让出 CPU。TC1 OTA/metadata 优先级低于 HTTP，OTA 期间 HTTP 响应额外让出 10 ms。监听 socket 所属线程将 OTA 期间 HTTP 并发上限收紧到 A1/M1 的 4、TC1 的 2，结束后恢复普通上限。TC1 分配 TLS 前另留 500 ms 排空旧连接；这减少了瞬态占用，但尚未使压力更新通过。

## TC1 修复

旧固件签名上传返回 HTTP 500 的原因已在 SDK/适配层定位：MW300 的 BOOTLOADER 分区 `owner=MICO_FLASH_NONE`，原适配层通过它读取启动分区表必然失败。
当前实现通过 SDK 提供的 APPLICATION/OTA_TEMP 验证运行/被动槽地址和容量，保留镜像检查，由 SDK 完成启动切换；提交返回不确定错误时保留锁，防止覆盖已提交镜像。

用户明确授权后，曾用 unsigned 上传引导 TC1；后续候选均已重新通过**签名上传 202 + 实际重启 + 配置保留**验证。unsigned 引导的例外没有进入 GitHub 下载流程。

TC1 SDK 的 HTTP 栈原为 24576 B，现为 12288 B。同一设备重启后 heap 从诊断构建的 26224 B 增至资源构建的 38504 B，符合约 12 KiB 的回收量（读数含运行时波动）。GCC `.su` 已生成；例如 httpd_main 96 B、HttpV1Command/Reply 各 24 B、uECC_verify 536 B、tc1_ota_finish 632 B；这些是单帧数据，不能相加成完整最坏调用链或代替运行时栈水位。

## 最终构建资源

| 型号 | 应用 payload | 应用容量 | 扣除头部后的余量 | ELF BSS |
| --- | ---: | ---: | ---: | ---: |
| A1 | 211812 B | 262144 B | 50324 B | 33604 B |
| M1 | 186444 B | 262144 B | 75692 B | 27508 B |
| TC1 | 240236 B | 282624 B | 42380 B | 14008 B |

A1/M1 相较实施前同基线 payload 分别增加 31404/31356 B；不是直接把独立 TLS 链接数字当成整机增量。TC1 旧设备和当前构建基线不同，不给出未经同口径重建的增量。

TLS context 4552 B + 记录缓冲 2048 B = 6600 B；传输对象约 536 B，加上 6144 B 下载线程，基础动态预算约 13280 B，另需 RTOS、网络、分配器和请求瞬态。
日志记录从 76 B × 64 = 4864 B 降到 20 B × 64 = 1280 B，三型号均由 ELF `records=0x500` 确认，节省 **3584 B RAM**。
A1/M1 的联动栈预算 12 KiB；实际 heap 会受 RTOS 延迟回收、已存在请求和网络缓冲影响，不把每次读数都解释为同样的净收益。

## 前端及发布物

独立前端工作树 `build/frontend-source`，分支 `codex/ota-tls-ui`；当前提交 `a25d81c80509052b82fcd531a97dee1af5b1375f`。
新增 GitHub Pages HTTPS、境内 HTTP、境外 HTTP 三个检查源按钮，选择源后下载并安装、设备端 HTTPS 发布说明、下载终止及 OTA 小型状态轮询。

前端已推送分支并发布 [候选 prerelease](https://github.com/phicomm-io/phicomm-community-frontend/releases/tag/frontend-a25d81c80509052b82fcd531a97dee1af5b1375f)，固件 lock 固定该提交和归档 SHA-256 `f9d2d2dc6b9649fe1e367a093507d7281c2c4aa6aa8e14ff4cbf7d3669dc0b1c`。远端重新下载比对通过。未发布稳定固件。

测试站点 [GitHub Pages](https://fujr.github.io/phicomm-ota-tls-test/) 的 A1/M1 测试提交 `09f54a565cf866c30574f4fd105dd8c6afd2d8ec`、TC1 测试提交 `32fa9753b11c0d4caf0ed3ff8fd36719c30ef9be` 均已由 Pages 构建成功。
最终镜像分别位于不可变的 `/runs/ota-20261016-admission-2` 和 `/runs/ota-20261016-tc1-3` 路径；远端整包下载 SHA-256 与本地签名候选逐一相符。
测试镜像与生产 HTTP 镜像分开；两个 HTTP 镜像只查询其现有 v0.9.5，没有向其发布候选镜像。

## 检查与证据

构建入口 `./build.sh`；测试时设置 `PHICOMM_OTA_GITHUB_BASE_URL=https://fujr.github.io/phicomm-ota-tls-test/runs/ota-20261016-admission-2`，TC1 最后一轮单独构建使用 `/runs/ota-20261016-tc1-3`。默认生产地址仍为 `https://phicomm-io.github.io`。
证据保存在 git 忽略的 `build/ota-evidence/`，不提交设备配置、签名私钥、构建产物和截图。

- 三型号整机构建、A1/M1 worker 栈预算检查通过。
- `bash tests/run.sh`、`bash tests/ota/run.sh`、`python3 tests/test_archive_pages.py` 通过；TC1 测试模拟真实 SDK 中不可读取的 BOOTLOADER 分区。
- `python3 tests/test_ota_fetch.py`：生产传输代码 + 真正 OpenSSL TLS + ASan/UBSan，5 个测试方法通过，覆盖 HTTP/TLS、Range/重连、错误响应/长度、限制、卡住的 HTTP/TLS/握手取消。BearSSL 归档自身未加 sanitizer。
- 前端三型号 JSDOM 常规和 OTA 模式测试通过。真实浏览器结果与后端/API 测试分别记录。
- `tests/device/tls_ota.py` 提供三源检查、HTTPS 说明、HTTP/TLS/握手卡住取消、正文中取消、坏签名、错误型号、截断、自动超时和 6 客户端压力更新。
- `tests/device/ota_fault_server.py --images build/ota-test-site/releases/v0.9.5 --ports build/ota-evidence/fault-ports.json` 启动故障服务器，其 TLS 证书自签且域名不匹配，证明设备不依赖证书信任校验。
- `tests/device/ota_browser.cjs` 需要 Playwright：`node tests/device/ota_browser.cjs MODEL IP FIXTURE_HOST PORTS_JSON DEVICE_ID`。真实设备 UI 包含三源查询、HTTPS 说明、网址下载和终止恢复。

## 最终候选设备验证

| 用例 | A1 .111 | M1 .110 | TC1 .102 |
| --- | --- | --- | --- |
| GitHub HTTPS 最新版本、发布说明 | PASS | PASS | PASS |
| 两个现有 HTTP 镜像最新版本查询 | PASS | PASS | PASS |
| HTTP 无响应、HTTPS 无响应、正文停顿、TLS 握手停顿取消 | PASS | PASS | PASS |
| 错误签名、错误型号、截断、自动超时后恢复 | PASS | PASS | PASS |
| 完整包内容篡改、Flash 读回/镜像校验拒绝 | PASS | PASS | PASS |
| 失败后 MQTT 可用，A1/M1 联动 worker 重建 | PASS | PASS | PASS（无联动 worker） |
| 真实浏览器三源按钮、HTTPS 说明、下载取消 | PASS | PASS | PASS |
| 本地 HTTP 完整签名包下载、提交、重启、配置保留 | PASS | PASS | PASS |
| GitHub HTTPS 完整签名包下载、ready 提交、重启、MQTT | PASS | PASS | PASS |
| 六个 HTTP 客户端持续请求时 GitHub HTTPS 更新 | PARTIAL | PARTIAL | FAIL |

基础矩阵各 12 项；A1、TC1 的结果文件还包含第 13 项压力安装受理，不能把受理算作安装成功。
完整包篡改测试在 `body-readback/`：A1/M1 已通过信封签名，随后 Flash 读回 SHA-256 校验失败；TC1 到 100% 后镜像校验失败，均未提交或重启。
真实浏览器分别保存 `browser/results.json`、`sources.png`、`notes.png`、`cancel.png`，没有页面脚本错误。
基础故障场景的取消到恢复耗时：A1 1.19–1.91 s、M1 0.17–0.56 s、TC1 0.30–1.63 s。网页点击终止到恢复分别约 1.92/2.99/1.43 s；这不是六客户端洪泛中的取消时延保证。

最后一次无附加客户端的 GitHub HTTPS 签名更新均明确采到 ready/committing，重启后设备身份一致、OTA idle、MQTT 已连接，A1/M1 联动 worker 运行。记录位于各型号的 `final-https/`。

| 型号 | 下载受理到重启/MQTT 恢复 | 传输钩子 heap_min | 控制端采样最小空闲 heap |
| --- | ---: | ---: | ---: |
| A1 | 144.59 s | 28252 B | 28548 B |
| M1 | 159.93 s | 43820 B | 40468 B |
| TC1 | 149.89 s | 10464 B | 13168 B |

两种采样时机不同，均不是连续全局堆水位或最大连续可分配块；尤其 TC1 的余量仍较紧。
最后重启后继续检查到 A1/M1/TC1 uptime 分别为 109.19/94.31/100 s，期间没有再次复位，空闲 heap 分别为 30740/48196/40672 B；三台 MQTT ready，A1/M1 联动运行且 watchdog healthy。TC1 未提供同一 watchdog 状态字段，不将其记为该字段验收通过。证据为 `final-runtime-before-soak.json` 与 `final-runtime.json`。

A1/M1 最终压力轮的控制接口频繁被连接重置；M1 捕获到 100% validating，随后两台均重启并恢复正常，但没有采到 ready/提交成功。由于是同版本重装，重启不足以区分成功更新与异常复位，严格脚本失败，不能记为 PASS。
TC1 最后收紧到 2 并延迟 TLS 分配后，仍在 0% 返回 Invalid HTTP status；heap_min=14200 B，失败后恢复 idle/MQTT。此结果也表明不能把所有压力故障都归结于堆不足；TLS/HTTP 失败点与 SDK 监听并发行为仍需进一步定位。

上述压力为六个客户端每次请求后等待 80 ms，交替调用 status.get/config.get，OTA 期间仍持续施压；不是外部 MQTT 洪泛或全部硬件功能的组合压力。
两个公共 HTTP 镜像没有部署测试候选，因此公共镜像完整安装标为 NOT_RUN；成功的 HTTP 完整安装来自本地故障服务器 `/valid/MODEL` 的已验签候选。

复现无附加客户端的 GitHub 签名更新可使用 `tests/device/tls_ota.py ... --load-only --install --load-clients 0`；去掉 `--load-clients 0` 则默认使用六客户端压力。必须提供目标 IP、型号、device_id、故障服务器地址和 ports JSON；脚本在操作前核对设备身份。此脚本的重启判定还要求先观察到 ready，避免把同版本的异常重启误记为更新成功。

## 压力迭代说明

早期未限制 HTTP 并发时，A1 一轮压力更新在 100% 后校验失败，记录到 heap_min=1580 B；TC1 一轮在约 2% 中断。两者均自动恢复，A1 联动 worker 与 MQTT、TC1 MQTT 可用。A1 的错误当时被通用错误覆盖，因此不能把缓存混用或内存不足断言为校验失败的唯一原因。现保留具体错误，并给每次候选使用新的不可变 Pages 路径。

把并发收紧到 2 后，A1、TC1 压力轮观察到重启与正常状态，但未采集到完整提交阶段，严格脚本判为未通过；M1 正文取消和一次上传出现连接重置。不能把这些轮次记为完整验收。A1/M1 最终上限调整为 4；TC1 上限 4 的压力轮仍在 0%/2% 中断（最低记录约 5 KiB），故最后回到 2 并增加连接排空时间。最终候选结果见上表。

## 边界

这是针对已实测 github.io 行为的极小 TLS profile。仅 RSA TLS 1.2；不支持 ECDHE/TLS 1.3、重定向、chunked、压缩或大于缓冲的加密记录。Range 不是 TLS 最大记录协商保证，站点行为变化时会有界失败。
TLS 上下文已测量，完整 TLS/SDK 调用链的运行时栈水位、最大堆分配连续块、掉电恢复、Wi-Fi 断网重连后继续下载，以及外部 MQTT 客户端的 OTA 控制矩阵尚未验收。取消/失败后重新发起从头下载。
当前仅为开发工作树候选；压力更新尚未通过验收，未合并或发布正式稳定固件。生产 Pages 的小清单需随归档流水线部署后才可使用默认源。
