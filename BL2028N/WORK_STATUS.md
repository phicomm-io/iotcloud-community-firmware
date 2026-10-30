# S30-V1B 开发检查点（2026-10-30）

## 当前集成回归（2026-10-30）

已将设备适配纳入 `hotfix/rc4-selected-integration`。公共固件来源固定为 `d68cd83d`，公共前端为 `5ba16da4`；60 个适配文件已从冻结来源及补丁以零 fuzz、零 offset 重建并逐字节核对。原开发分支及下列历史测试记录保留，不代表当前候选已刷入。

本轮同步公共 HTTP 两等待槽、串行读取及能力裁剪、无密码免会话访问；完整业务状态使用事件失效缓存，计量/网络变化和采样过期均触发更新，诊断改用共享编号的独立接口。原生 MQTT 与 HA 分别记录发送进度，补入接收错误恢复和可选缓冲缩小失败处理。OTA 原生 HTTP 读取超时及错误传播已修复。

当前验证：16 项核心 ASan/UBSan、9 项真实 HTTP/MQTT/UDP 套接字、配置迁移与计量启动回归通过；Chromium 实际公共 HTTP 页面完成表单、未保存编辑保留、无密码刷新、手机布局和主题切换。原生 HTTP worker 的 6 种真实 TCP 下载情形通过，包括停顿、提前断流及随后完整 RBL 重试。上述主机测试不替代芯片实机验收。

设备经用户重启后已恢复到 `0.9.27-299121-dirty`，Wi-Fi、继电器和有效计量正常，升级状态已退出。集成候选 `1.0.0-rc.3-fda605` 已由共享 GCC 5.4.1 构建，RBL 为 476544 字节，完整结构及来源校验通过；后续日志修正将生成对应提交的新候选。实机尚未完成刷入，在确认新版本、设置保持和失败 OTA 恢复前，不标记为硬件通过。

### 直连诊断补充

已直接从测试主机向设备提供完整 RBL。TCP MSS 1460、512、255 均只有约 1 KiB/s，22 秒采样约 5%；100 ms 一次 ICMP 保活也未改善，期间 ping 约 3–8 ms。缓慢探测均在旧版 120 秒期限前主动中断，设备自行退出维护状态，未重启，配置与输出保持。不能把服务器已发送到本机缓冲区的字节当作设备已接收字节。

实际 TCP 日志证明旧版每收到 255 字节打印一条信息日志、每写入 512 字节打印地址。独立代码检查发现：`set_printf_port(1)` 只改变 SDK printf，OpenBeken 缓冲日志仍默认 `UART2_PORT` / index 1，与计量的 4800 8E1 UART2 冲突。这是已确认的端口错误；其对 OTA 吞吐的贡献尚需芯片对照，不能仅凭时间相关性归因为全部下载瓶颈。

本轮修正原生日志默认端口为 UART1 / index 0，保留启动时 SDK printf 的 UART1 设置；下载逐块日志降至 debug，进度与错误日志保留。`test_logging_uart.py` 对真实原生日志发送函数和默认选择器编译运行，覆盖计量 UART 满、日志 UART 满、继续排空及关闭串口日志四种情形；修正版通过，原始 UART2 默认值作为负对照失败。完整 HTTP worker 的 6 种真实 TCP 情形在修正后仍通过。此修复只能作用于新固件，不能在刷入前改变当前旧固件。

旧版短基线：2 个读取者，各 500 ms 一次，20.46 秒共 76 次，零 busy、零传输失败，P95 149.56 ms，状态正文 1353 字节，最低采样空闲堆 10728 字节。仅适用于这一短读负载，尚无新固件对应值。

当前私有运行目录：`../phicomm-messagepack-lab/private/main-integration-20261030/s30-integration-20261030/`。保留下载失败前后、重启恢复后的私有配置快照、来源 tar、构建与验证记录。下面为原分支检查点历史。

工作保存在 `feat/bl2028n-openbeken`。代码、公共服务/前端来源及适配补丁、测试、双语设备说明均已提交；最新候选固件构建通过，最终实机 OTA 未通过。`main` 保持原状，没有推送或发布。用户指定的合并条件仍为最终测试通过。

## 源码与固件对应关系

| 提交 / 版本 | 内容 | 验证边界 |
| --- | --- | --- |
| `2b483aa98bf0d15f9af33f7b2d9c04451801a892` | OpenBeken 移植、公共服务/前端、身份、鉴权、OTA 内存适配 | 已有主机及实机服务记录；不代表后续候选固件通过 |
| `42236a4b56efbe283e94e7ab1a7f71d4da7bfc65` | 蓝灯改为高电平亮 | 用户随后确认输出开启时蓝灯常亮 |
| `af6d4e0fc0f5af97b5ce70e3d47296107d86ed2b` | TC1 长按计时灯语 | 用户确认每秒一次、松手恢复；保存了该版候选归档 |
| `299121020c4f5245739571635044466f3d1ac1da` / `0.9.27-299121-dirty` | 计量改到 UART2/P1，日志留在 UART1；设备说明 | 最近一次确认运行的设备版本；真实负载计量、实际 HTTP 及浏览器验证通过 |
| `e4bb9934` | 峰值/范围统一一位小数 | 已进入下行候选镜像，尚未实机验收 |
| `13e52e6e6e46b8f517d9475d7ec0df43572ec561` / `0.9.27-13e52e-dirty` | 更新前端适配来源校验 | GCC 5.4.1 构建、固件结构校验通过；实机 OTA 失败，未安装成功 |

`-dirty` 按仓库规则保留，不能改标为干净发布。此检查点增加的是整理文档及离线研究工具，不声称已为检查点提交重新构建固件。运行框架固定 OpenBeken `7b754d6fc80b72115ece31fcf4527f011f90deef`，BK7231N SDK `07fd9e0e2da0aabaddab00be2a897589be235158`。

## 已保留的验证

私有证据根目录记为 `$S30_RUN`：`../phicomm-messagepack-lab/private/bl2028n-flash-20261029/unified/`，相对于仓库根目录。原始备份、设备标识、网络配置和原始抓取不进入 Git。

| 检查 | 结果与限制 | 私有证据（相对 `$S30_RUN`） |
| --- | --- | --- |
| 产品及真实套接字回归 | 15 项核心 ASan/UBSan、8 项实际 HTTP/MQTT/UDP 用例通过 | `host-meter-uart/host-tests.log`、`network-tests.log` |
| 启动及计量 UART | 正常/安全模式、已配网/未配网顺序通过；测试实际产品计量定时器与 UART2 分片帧 | `native-meter-uart-startup/result.json`、`host-meter-uart/` |
| 公共来源与补丁 | 59 个文件从冻结来源无模糊匹配重建一致 | `source-patch-reconstruction.json` |
| 工厂身份及服务 | 实际 MAC、DHCP hostname、HTTP/MQTT/UDP、鉴权、此前 OTA 成功及错误恢复 | `shared-services-acceptance.json` 中逐项索引，保留各自版本边界 |
| 计量修正版 OTA | `299121` 升级后自动重连、身份及设置核对通过 | `hardware-meter-uart-ota/result.json` |
| 真实负载计量 | 58.82 秒内 30/30 有效样本、有效帧 +565、坏帧无增长、累计电量增长；输出持续开启 | `hardware-meter-live-fixed/result.json` |
| 实机 HTTP / 前端 | MP/CBOR、JSON 业务拒绝、完整前端、空密码重复访问；Chromium 桌面/手机布局、实时功率和默认校准提示；录像已解码检查 | `hardware-meter-final-http/result.json`、`hardware-meter-browser/result.json` |
| 最新候选构建 | `13e52e` 关闭启动诊断，固件 CRC/加解密/布局/符号/来源校验通过 | `build-run-shared-services/build-manifest.json`、`firmware-validation.json` |
| 最新候选实机升级 | **失败**：未运行新版本，超时后仍在升级状态 | `hardware-final-meter-ui-ota/result.json`、`status-after-failed-transfer-private.json` |

真实负载读数约 220.77–221.61 V、0.0637–0.0904 A、6.86–13.12 W。用户估计的约 20 W 不是参考仪表读数，不能用于调整校准系数或认定精度。当前 `calibrated=false`、`accuracy_verified=false`。短按、红蓝灯、长按重置和每秒提示已有用户反馈；5 秒/10 秒边界及配置保留主机回归与逐项实物验收须分别记录。

## 尚未解决的 OTA 阻塞

`299121` 下载 465728 字节的 `13e52e` RBL 时，约 120 秒停在 32%，360 秒等待结束仍为 `ota_active=true` / `downloading`。设备继续响应只读 HTTP，负载及计量继续工作；未确认新版本启动。此情况不能标为升级成功或失败清理成功。

代码与时序支持的原因：`build.py` 对原生 `utils_net.c` 的覆盖使 `HAL_TCP_Read` 在到期时返回 0；原生 `http_client.c` 的 `httpclient_recv` 又把 0 当成功并返回零读取长度，后续正文接收仍有剩余长度，因此可能反复无进展读取而不触发错误回调。现有 `tests/test_http_timeout.py` 只覆盖底层 TCP/计时，未覆盖完整 HTTP 客户端退出和维护状态释放；需要完整路径回归验证这一判断。

接续顺序：

1. 在完整原生 HTTP 客户端处理到期/无进展，确保调用错误清理并释放 OTA 维护状态；同时检查已读部分数据和连接关闭的语义。
2. 使用真实慢速/停顿 HTTP 服务验证总期限、错误回调、UDP/MQTT 恢复、再次升级及正常完整下载；不能只提高超时掩盖退出错误。
3. 恢复设备后，重新构建并进行最终实机 OTA、版本、身份、网络、鉴权、前端及计量回归。设备最后记录为原电源及负载供电，UART/外部直流已断开；这种供电状态下不可恢复串口接线。
4. 单独安排参考仪表校准和未完成的实物边界项。验收通过后才按授权合入 `main`。

## 原厂免拆迁移研究

已找到 ORVIBO 云 TCP 及 AndLink MQTT 到 HTTP 下载/写入 OTA 分区的静态调用链；**尚无原厂固件实机无线迁移成功证据**。原厂 OTA 起点 `0x132000` 与社区版当前 `0x12a000` 不同，bootloader 包格式/校验和实际入口可达性仍待确认，不能直接把当前 `.rbl` 交给原厂下载器。

分析地址、输入 SHA-256、可复查脚本和后续验证步骤见 [stock-ota.md](research/stock-ota.md)。首次安装的公开说明继续仅提供拆机串口刷机。

## 制品与现场资源

- 已验证计量版本归档：`$S30_RUN/meter-uart2-first-validated-299121.tar.gz`，SHA-256 `acab0dd6146b1cc09846dc98fbe238f53a9afa77393ff09dc9ad781801444932`。
- 本次候选及源码归档：仓库 `private/s30-checkpoint-20261030/`，包含 `13e52e` 的七种固件、ELF、构建清单、覆盖补丁、日志、原提交源码及检查点材料；`artifact-index.json` 和 `SHA256SUMS` 标明版本、校验与验收状态。完整原厂备份仍留在原私有目录，不复制入 Git。
- 活跃资源清单：`$S30_RUN/ACTIVE-RESOURCES.md`。唯一 SDK 构建副本暂留以修复上述 OTA 阻塞，任务热点恢复配置暂留用于设备恢复；无新增工具链或临时 Git 工作树。
- 未纳入本任务的 A1/M1/TC1 构建脚本、`tools/prepare_sdk.py`、根目录已有私有内容及未知来源下载文件保持原状。
