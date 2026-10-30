# S30-V1B 原厂 OTA / 免拆迁移分析

2026-10-30 实机更新：**本机 S30-V1B 原厂 `2M.1220` → 社区固件的网络 OTA 已通过**。使用正常 CoAP CGW 配置和 TLS MQTT 文件命令，原厂下载器完整接收 RBL，原厂 bootloader 安装并启动社区应用；重新配网、实机网页和后续社区 OTA 均通过。完整 Flash 回读证明原厂启动区及工厂 MAC/RF 不变。仅验证此型号和此原厂版本，不宣称其它版本或刷回原厂可用。详细过程、制品校验值和边界见[实机记录](stock-ota-live-test.md)及[结构化结果](evidence/stock-network-ota-live.json)。

过程、纠错记录与验证路径另见[探索与验证记录](stock-ota-exploration-log.md)。原厂/社区分区差异、容量实测与双向切换条件见[统一分区可行性](stock-partition-layout.md)。

## 当前目标：原厂单向免拆安装社区固件

按用户最新要求，只验收本机原厂 `2M.1220` → 社区固件；不要求社区刷回原厂，也不以双向切换作为前置条件。原厂恢复包仅保留为私有研究/恢复材料，不作为本目标的发布依赖。

推荐路径为：原厂网络命令触发下载 → 将社区 RBL 写入原厂 download 区 → 原厂 bootloader 安装社区应用 → 社区启动及后续社区 OTA。保留原厂 bootloader，不要求通过原厂应用 OTA 替换启动区。社区应用尾块、后续 OTA 地址和配置区域仍须与该启动器一致；取消回刷要求不会消除这些布局约束。首次从原厂安装允许清空业务配置、重新配网，无需先迁移已有社区配置，但必须保留本机工厂 MAC/RF。

上述单向路径已在本机完成。测试前用串口恢复原厂基线，串口也用于初始入网诊断、日志和最终只读回读；验收升级阶段未使用串口写下载区、串口 OTA 命令或调试器调用。后续社区 OTA 采用同一源码重新打包的候选，实际安装及配置保留已验证，跨版本配置迁移未验证。原厂未配网设备的完整首次入网、其它原厂版本与安装中断电场景仍需另测；本结果也未合入 `main` 或发布。

## 实机前的离线研究记录

下文保留早先静态分析与当时的待验证项；其中“尚未实测”“当前版本”等表述属于实机前检查点。当前实物状态和验收结果以上方实机记录为准，局部仿真不能替代对应的实物证据。

## 输入与复查

仅对应本机 S30-V1B / BL2028N / BK7231N 的双读一致原厂备份。备份中显示设备名 S30M、硬件 1.0.0、业务软件 20231008、SDK v0.0.7；不能外推到其他型号或原厂版本。

| 输入 | 长度 | SHA-256 |
| --- | --- | --- |
| 原厂完整 Flash（两次一致） | 2097152 | `1252da1ea8fd19fa5341fb6dee28b4ad6c1599394981bc5b60cd5d2bd0c2e610` |
| 已解密、去 Beken 块 CRC 的应用 | 1079392 | `6cd7246861ea44f277f25bc61b2f236cec7c5f4db3c487cdb177c8bbc9c9e723` |

下文地址均为解码应用的逻辑地址，文件偏移为地址减 `0x10000`；不是串口写入偏移。业务代码按 Thumb 解码，开头的 ARM 启动代码不能直接按 Thumb 解释。私有解码应用在 `$S30_RUN/hardware-live-meter/factory-app-decoded.bin`，详细反汇编在 `$S30_RUN/stock-ota-analysis/`，根目录定义见[工作记录](../WORK_STATUS.md)。这些文件包含原厂数据，不进入 Git。

离线复查工具只核对这两个已知 SHA 的输入中的直接调用、回调指针、分区表、RBL 尾块及明确的公开字符串，不提取或输出产品密钥、账号或设备身份，也不连接设备：

```sh
source ~/.cache/phicomm/env.sh gcc5
python3 BL2028N/research/stock_ota_inspect.py \
  --app "$S30_RUN/hardware-live-meter/factory-app-decoded.bin" \
  --flash "$S30_RUN/../original-flash-read1.bin" \
  --output /path/to/private/stock-ota-static.json
```

`--flash` 可选。给出时额外校验 bootloader 与应用两个 RBL 尾块（magic、名称、算法、头 CRC32）、`fnv1a(原厂应用前 0xEADFF 字节) == 尾块 hash` 这一哈希约定、`01PEdownload` 分区描述符（0x132000/0xAE000）、原厂 OTA 字符串集合，以及相关签名名称是否出现在字符串中（不能据此证明签名校验不存在）。AES 密钥材料只报告存在与否，不输出内容。

使用共享 Python 中的 `capstone==5.0.6`，不另建运行时。仓库内的 [stock-ota-static.json](evidence/stock-ota-static.json) 是该工具的实际输出。它只能复查静态定位，不能代替完整路径仿真或实机验收。

第二个工具用共享 Python 里已有的 `unicorn` 直接仿真原厂 bootloader 的自定义校验（`0x9B74`），判定"已暂存的应用载荷会不会被拒绝"，不连接设备：

```sh
python3 BL2028N/research/stock_ota_emulate.py \
  --boot "$S30_RUN/../original-boot-zero-decoded.bin" \
  --app  "$S30_RUN/build-run-shared-services/firmware/BL2028N_app_<version>.bin"
```

输出为逐用例的 `accepted`/`messages` JSON，属于静态仿真，仍需实机验收。

## 两条网络入口

### ORVIBO 云 TCP

`0x18444` 初始化连接对象，把 `0x188b5`（Thumb 函数 `0x188b4`）注册为接收回调。连接状态处理在 `0x1c742` 一带，`0x1c792` 调用 TCP connect `0x2c828`；默认主机为 `homemate.orvibo.com`、端口 10001。当前定位的是设备发起的云连接，尚未找到同一命令的本地监听入口；域名/地址的持久配置及缓存仍须跟踪，不能认定更换 DNS 就一定生效。

回调 `0x188b4` 中可见的帧结构：

| 偏移 | 内容 |
| --- | --- |
| 0–1 | `hd` |
| 2–3 | 大端总长度，解析器上限 `0x2800` |
| 4–5 | `pk` / `dk` 密钥模式 |
| 6–9 | 密文正文的 CRC32，大端 |
| 10–41 | 32 字节会话标识 |
| 42 起 | 解密后作为 JSON 解析的正文 |

CRC 函数 `0x19c54`，密钥设置 `0x1b264`，解密 `0x1b618`，JSON 解析 `0x16e58`。`pk` 分支选择固件内的共享 AES 密钥，`dk` 分支选择运行态密钥；日志里的 “public key” 不能解释为非对称公钥或固件数字签名。AES 模式、填充及完整帧还没有通过已知向量或离线仿真验证。

`cmd=76 (0x4c)` 分支在 `0x18dc6`，要求 `type`、`url`，`type=4` 时于 `0x18e0e` 调用 `0x19c14`。结构候选如下，**尚未发包验证**：

```text
{"cmd":76,"type":4,"url":"http://controlled-host/firmware"}
```

静态路径中未观察到此分支之前强制比较会话标识或完成登录；可选 `uid` 存在时会比较设备 UUID。这只形成待验证假设，不能声称未登录命令已被接受。这里的 JSON 属于原厂协议研究，社区固件业务仍仅支持 MessagePack/CBOR。

### AndLink MQTT

回调 `0x15510` 经 `0x14e6c` 注册，Paho 接收桥为 `0x170b4`；订阅主题模板 `/device/%s/downward`，上行 `/device/%s/upward`。

当 JSON `function` 为 `file` 时，`0x15618` 一带先发送文件响应，检查存在 `fileMode` 和 `fileUrl`，再于 `0x15664` 把 URL 传给 `0x19c14`。该分支未观察到对 `fileMode` 值的额外检查。它依赖原厂 MQTT 会话、broker 和设备路由可控性，不等于本地开放 MQTT 升级口。CGW 配网解析和通用 UDP 处理尚未证明可直接触发 OTA。

## 共同下载、写入及启动边界

| 逻辑地址 | 静态作用 |
| --- | --- |
| `0x19c14` | OTA 忙标志与 600000 ms 清标志定时器；`0x19c30` 调用下载器 |
| `0x5e838` | HTTP OTA 下载，两个 1024 字节缓冲区、端口 80、GET、请求超时参数 20000 ms |
| `0x5e880` → `0x28b04` | HTTP 请求；下载结果为 0 时走重启路径 |
| `0x5e88e` → `0x5d824` | 重启调用 |
| `0x28224` | Flash 下载初始化，选择分区编号 2，解除保护、准备缓冲区 |
| `0x28328` / `0x28148` | 1024 字节分块处理、4 KiB 边界擦除、写后读回比较、分区边界检查 |
| `0x3edd8` | 串口 CLI 的 `http_ota [url:]`；这本身不证明免拆入口 |

分区查询 `0x3cfd4` 使用启动时从 `0xe3b84` 复制的表。表中的声明值：

| 编号 | 名称 | 物理起点 | 长度 |
| --- | --- | --- | --- |
| 0 | Bootloader | `0x0` | `0xf000` |
| 1 | Application | `0x11000` | `0x143000` |
| 2 | ota | `0x132000` | `0xae000` |
| 3 | RF Firmware | `0x1e0000` | `0x1000` |
| 4 | NET info | `0x1e1000` | `0x1000` |

Application 声明范围与 ota 存在重叠，必须继续核对 bootloader 实际读写与 RBL 描述，不能把这张表直接当作制作镜像的安全写入边界。社区版当前 OTA 起点为 `0x12a000`，也不能据已有社区版 OTA 成功记录，认定原厂下载器可以接受同一个 `.rbl`。

本次应用路径尚未看到 URL 命令附带固件签名/摘要参数；bootloader 侧的包格式、压缩/加密、CRC、装载目标与签名要求见下文第二节。`signature` 字符串的已定位引用 `0x1910c` 属于登录/上报路径，不能作为 OTA 有签名或无签名的证据。HTTP 下载和 CRC 也不能等同于固件来源认证。

## 第二轮静态分析：原厂 bootloader 的接受条件

### 前提更正（必须先读）

本机当前运行的不是原厂 bootloader。首次拆机写入的是自 0 起的 1220464 字节 QIO 镜像（`flash.py` 写入 0 到 0x12a000），其中 0x0–0x11000 的 bootloader 是 OpenBeken 上游文件（尾块名 `bootloader`、版本 `1.00`、raw 59932 字节）。原厂 bootloader（尾块版本 `2M.1220`、raw 54464 字节）从未参与过任何一次 OTA。因此**此前"社区 OTA 成功"只证明上游 bootloader 接受社区 `.rbl`，不能证明原厂 bootloader 接受**。第二节的结论只覆盖原厂 bootloader 的静态接受条件，仍未实机验证。

### 原厂 bootloader 的 OTA 实现

原厂 boot 区（0x0–0xFF00）为 34 字节分块（32 字节数据 + 2 字节大端 CRC16，多项式 0x8005）编码，末 102 字节为 96 字节 RBL 尾块。静态事实：

| 项 | 事实 | 位置 |
| --- | --- | --- |
| OTA 库 | RT-Thread OTA 0.2.4（rt_ota） | 字符串 `RT-Thread OTA package(V%s)`、`0.2.4` |
| 分区表 | 名为 `download`（0x132000，0xAE000，物理 `beken_onchip`）、`app`（逻辑 0x10000/0x110000）和 `bootloader`，后两者为 `beken_onchip_crc` | 0xD400 起 `01PEdownload` 描述符 |
| 包格式 | 96 字节头：`RBL\0`、包版本、算法、时间戳、名 16、版本 24、SN 24、载荷 CRC32、FNV-1a、raw 长度、包长度、头 CRC32 | `rt_ota_part_fw_verify` 0xA134、`part_fw_hash_verify` 0x9344 |
| 压缩/加密 | 无加密、AES256 与 QuickLZ/FastLZ/GZip 组合（`fw_algo_support_check`） | 0xA504 起，错误串 `Not supported this compression/encryption algorithm` |
| 完整性 | 逐 32 字节 FNV-1a（基准 0x811C9DC5、乘数 0x01000193）与头内 hash 比较；另有载荷 CRC32 | 0x9344 循环 |
| 非对称签名 | 未检出 `RSA`/`ECDSA`/`sha256`/`signature` 字符串；该检索不能证明没有对应运算 | 字符串检索，完整校验路径待验证 |
| 对称密钥 | 存在 Beken 默认 AES 密钥材料（与社区打包相同来源），本工具只报告"存在"，不输出 | 0xC7C 起 |
| 升级判定 | `memcmp(下载区头, 应用尾块头, 96) != 0` 即升级；没有版本序关系，故可降级/重刷 | `rt_ota_check_upgrade` 0x99E8 |
| bootloader 保护 | `Can not upgrade bootloader partition!` | 0x92B0 |
| 自定义校验 | `part_fw_hash_verify` 对**纯固件前 0x400 字节**逐 32 字节调用 0x9B74；失败即 `Custom verify RAW firmware failed!` | 0x93B8/0x943C |

尾块实测同时证明了哈希约定：`fnv1a(原厂应用前 0xEADFF 字节) == 应用尾块 hash 0xD88CB993`，与社区打包脚本的 `assert fnv1a(raw)==fnv` 完全一致。原厂应用尾块为 `name=app`、`version=2M.1220`、raw 0xEADFF、pkg 0xEAE00；原厂 boot 尾块为 `name=bootloader`、raw 0xD4C0。

### 算法位与自定义校验（本轮已判定）

`fw_algo_support_check` 逐值分支实际得到的接受集合（算法名由各分支的 PC 相对取址解析）：`0x000` NONE、`0x002` AES256、`0x100` GZip、`0x102` GZip+AES256、`0x200` QuickLZ、`0x202` QuickLZ+AES256；`0x300`/`0x302`（FastLZ 系列）在本版本被直接拒绝（`Not supported FastLZ compress firmware`），低位 `0x1`（XOR）也被拒绝。也就是说本版本的位定义是 bit1=AES256、bit8=GZip、bit9=QuickLZ、bit8|bit9=FastLZ，而不是把压缩器放在低位。社区 `.rbl` 的算法字段实测为 `0x0102`，正好落在 GZip+AES256 上——解压器与密码学都不是靠假设，而是原厂代码里明确映射的。AES 材料也逐字节相同：原厂 boot 内的默认 key/IV 字符串常量与上游 boot 完全一致（即仓库 `verify_firmware.py` 已使用的 BK7231 默认值），本记录不重复抄写字节。

0x9B74 的自定义校验（原厂版本比上游多出 `Please use none crc image to build rbl file!` 与 `The application must link to the flash 64KB location!` 两条分支）已用 unicorn 直接仿真判定，不再依赖阅读推断。它的调用约定来自拷贝路径（`0x8AE4`/`0x72C4`）为 `rt_ota_custom_verify(part, offset, buf, len)`，且只对 `offset < 0x400`、源分区名为 `download` 的首次数据生效。仿真结论（`stock_ota_emulate.py`）：

| 用例 | 结果 |
| --- | --- |
| 社区应用的纯固件首块（`part=download, offset=0`） | **接受**（返回 0，无错误输出） |
| 社区应用 offset ≥ 0x400 | 接受（跳过检查） |
| 应用分区自检（`part=app`） | 接受（检查不适用于 app 分区） |
| 载荷已按 32+2 加 CRC16 帧 | 拒绝：`Please use none crc image to build rbl file!` |
| 当前分区名非 app | 跳过：`The current part name: … does not need to custom verity.` |

64KB 链接检查读的是载荷第 0x20 字节起的 32 位值（应用向量表第 9 个字），社区应用与原厂应用在这个位置都是 `0x106C0`，因此通过。本轮已纠正旧仿真器的 `app_size=0x143000`：应读取原厂 boot 的真实 app 描述符 `0x10000/0x110000`。两种应用各 13 个用例通过，包括链接字的上下边界；实测上界 `0x120000` 被接受、再加 1 被拒绝。**这只证明自定义校验放行对应纯应用载荷**；已加 CRC 帧和链接字越界等错误输入仍会被拒绝。

已核对包字段、CRC/FNV、算法映射、密钥常量、自定义校验和分区容量，但还没有执行原厂启动器的完整安装流程。`decrypt & decompress` 与写应用分区尚未端到端仿真，不能用局部函数通过代替安装启动成功。另有独立的布局阻塞：当前社区 download/尾块/配置位置与原厂不同，需统一后才能支持持续双向更新，详见[统一分区可行性](stock-partition-layout.md)。

### 应用侧下载不校验包内容

0x5E838 只做三件事：分配两个 1024 字节缓冲、发 HTTP GET（端口 80 明文，超时 20000 ms）、返回 0 即调用 0x5D824 重启。写入路径 0x28224 取分区编号 2（`ota`，0x132000）、0x28148 按 4 KiB 扇区擦写并回读比较，0x28398/0x28328 负责 1024 字节分块与重试。整条路径**没有** RBL 头、名称、版本、CRC 或签名检查，字节流原样落盘；能否启动完全由 bootloader 决定。

### 对"原厂固件是否有 OTA 更新社区固件的能力"的回答

| 结论 | 依据 |
| --- | --- |
| 有能力：存在从网络命令到下载、写 OTA 分区、重启的完整链路 | 0x18E0E/0x15664 → 0x19C14 → 0x5E838 → 0x28224 → 0x5E88E，已静态逐环核对 |
| OTA 目标分区与原厂 bootloader 的 `download` 分区同一（0x132000） | 应用分区表编号 2 与 boot 描述符 `01PEdownload` 一致 |
| 已定位 CRC/FNV、AES256+GZip 及自定义校验路径 | 不能仅凭缺少签名字符串，宣称完整启动链不要求签名；安装路径仍待验证 |
| 需要先控制下发方 | 云 TCP 10001 为明文 TCP + 应用层 AES（共享密钥在固件内），或重定向设备访问的 HTTP 主机；本地 UDP 命令表只有 `outletStatus`/`setTime`/`SystemControl`/`pwCutMemory`/`powerConsumptionClear`，**没有** OTA 命令 |
| 容量与自定义校验支持继续验证 | 本轮社区候选应用 664576 B、RBL 476320 B；原厂恢复 RBL 649504 B；均能放入原厂 712704 B 下载区，不代表已安装启动 |
| 仍不能宣称免拆可用 | 原厂 bootloader 从未实机跑过社区包，`decrypt & decompress` 与写应用分区未端到端仿真；需要具备串口恢复的实机验证 |

## 第二轮静态分析：栈溢出与可达性

按要求同时核查了"是否存在可调用 OTA 功能的栈溢出"。静态结果：

- **已定位一处可致控制流的栈缓冲溢出**：`0x15208` 用 `sscanf(item->valuestring, "%[^:]%*[://]%[^:]%*[:]%ud", sp+8, ...)` 解析 `mqttUrl`，`%[^:]` 无字段宽度，目的地 0x1525A 处为 `add r2, sp, #8`。该函数栈帧仅 32 字节（8 个 push 字），越界写入会覆盖保存的 r4/r5/r6 与 LR，函数返回即失去控制流。
- 入口链：`0x17950`（HTTP 响应解析）→ `0x17A54` → `0x15208`。输入是需要控制设备所访问 HTTP 服务器的 JSON 响应。
- 与 OTA 的关系：控制到 PC 后可以调用 `0x5E838`（`http_ota(url)`）或直接走写入路径，因此"能调用到相应功能"在静态层面成立；但**未做动态验证**，且前提（控制下发服务器）与直接伪造 OTA 命令相同，溢出并不降低该前提，只是多一条执行路径。
- 同批扫描的其它结果：全应用 21 处 `sscanf`、529 处栈目的拷贝；`0x17CE4`/`0x1D130` 的 `%[^/]`、`%[^:]` 目的地是按长度分配的堆缓冲（等效有界），`http_ota` 的 host 拷贝带显式 `Host str is too small` 检查，本地 UDP 命令处理只用 `"%d"` 写 int。故上述 0x15208 是目前选定性最强的一处。

以上均为只读静态结论：没有恢复原厂、没有发包、没有实机复现。

## 实机前拟定的验证顺序

1. 离线完成帧加解密/CRC 向量，仿真或隔离调用接收解析器，确认命令分支、登录前提和会话检查；不输出原厂密钥。
2. 跟踪连接配置，确认可控制的 AP/DNS/MQTT 是否能让**此版本**连接隔离服务器；先用无写入的响应验证协议。
3. 0x9B74 自定义校验已仿真判定接受社区载荷；下一步把判定推进到 `decrypt & decompress` 与写应用分区的端到端仿真（0x748C/0x880C 与 0x8184 写入路径），确认安装后的应用分区内容与 footer 自洽。
4. 在具备串口恢复条件的测试机上，先用原厂固件验证一次原厂正常升级，再验证原厂 → 社区固件迁移和重启身份/服务；最后才能形成公开免拆步骤。当前设备使用原电源带负载，不在本次整理中恢复原厂或接入串口。
5. 若后续要做溢出验证，只在离线仿真或具备串口恢复、且已书面确认的测试设备上进行，不放宽现有"首次安装仅拆机"的公开说明。

最终需要的证明是：原厂网络入口实际可达、目标包被原厂启动链接受、重启后社区固件运行且身份/射频参数保留。当前完成的是原厂 bootloader 接受条件与一处栈溢出的静态定位，仍未实机验证。
