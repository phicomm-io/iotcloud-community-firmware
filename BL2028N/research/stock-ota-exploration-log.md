# S30-V1B 原厂 OTA 探索与验证记录

本文记录 2026-10-30 两轮只读静态分析及随后实机验证的**过程、办法、走过的弯路和验证路径**。结论与对外口径以 [stock-ota.md](stock-ota.md) 为准；实际通过的升级路径、制品和验证边界见[实机记录](stock-ota-live-test.md)。

第 1–8 节及其后的离线复核条目保留当时状态，不追改历史结果。本页最后的实机条目记录后续完成的网络升级，不应把早先的“全部离线”理解为当前仍未测试。密钥字节、网络配置及原始设备身份不进入公开证据。

---

## 1. 输入与地址空间

| 输入 | 长度 | SHA-256 | 位置 |
| --- | --- | --- | --- |
| 原厂完整 Flash（两次读一致） | 2097152 | `1252da1e…c2e610` | 私有 `$S30_RUN/../original-flash-read1.bin` |
| 解码应用（去 Beken 块 CRC） | 1079392 | `6cd72468…` | 私有 `$S30_RUN/hardware-live-meter/factory-app-decoded.bin` |
| 解码 bootloader | 65536 | — | 私有 `$S30_RUN/../original-boot-zero-decoded.bin` |

地址约定：

- **应用**用逻辑地址，基址 `0x10000`，文件偏移 = 地址 − 0x10000；业务代码按 **Thumb** 解码。
- **bootloader**用物理地址，基址 0；主体按 **ARM** 解码，被 `blx` 调到的 SDK 辅助函数按 **Thumb** 解码。
- 两者都不是串口写入偏移。

本轮确定的物理布局：

| 区域 | 范围 | 说明 |
| --- | --- | --- |
| bootloader 镜像 | `0x0`–`0xFF00` | 载荷块到 `0xE20C`，之后 0xFF 填充，96 字节 RBL 尾块在 `0xFE9A`–`0xFF00` |
| 应用分区 | `0x11000`–`0x132000` | 载荷块到 `0x10A8E0`（raw `0xEADFF`），尾块在 `0x131F9A`–`0x132000` |
| OTA / `download` 分区 | `0x132000`–`0x1E0000` | 长度 `0xAE000`，原厂备份中为空（全 0xFF） |
| RF / NET info | `0x1E0000` / `0x1E1000` | 各 `0x1000` |

分区镜像编码：**32 字节数据 + 2 字节大端 CRC16**（多项式 `0x8005`、初值 `0xFFFF`）为一组，34 字节步进。

---

## 2. 工具与办法

只用共享运行时：`capstone==5.0.6`、`unicorn==2.1.4` 与共享 Python，不新装工具链。

自建索引器（脚本都在私有 `round2/`）：

| 脚本 | 作用 |
| --- | --- |
| `stockre2.py` | 应用（Thumb，基址 0x10000）索引：指令表、函数起点、字符串引用 |
| `bootarm.py` | boot（ARM，基址 0）索引：同上，含 `add/sub rD, pc, #imm` 与 `ldr` 字面量池 |
| `frames4.py` | 逐函数栈帧估算：push 字数 + `sub sp`/`add sp, rN`（含负字面量） |
| `args.py` / `triage.py` | 栈目的拷贝扫描（参数恢复）：全应用 529 处 |
| `sscanf.py` | 21 处 `sscanf` 调用点与格式串清单 |
| `bruterefs5.py` | 字面量池／PC 相对引用暴力扫描，解决线性反汇编失步 |
| `footer.py` / `fnvcheck.py` | 解析两个 RBL 尾块并验证哈希约定 |
| `emu_cv2.py` | unicorn 仿真原厂自定义校验（定型后并入仓库工具） |

方法要点：

1. **字符串 → 函数反查**：先把 `ldr rX,[pc,#imm]` 池值与 `adr` / `add rX,pc,#imm` 目标解析成字符串地址，再按“函数起点 ∪ BL 目标”求函数归属；
2. **调用图上溯**：从 `sscanf`/`memcpy`/`http_ota` 等叶子一路 `callers()` 追到网络回调，判断可达性；
3. **参数恢复**：轻量抽象解释（寄存器 = 栈偏移／常量／字符串／未知），筛出“栈目的 + 变量长度”的拷贝；
4. **判定性仿真**：对无外设依赖的纯逻辑函数用 unicorn 打桩执行，用日志字符串反推走了哪个分支。

---

## 3. 探索过程与五次纠错

这一节记录**走错的判断**，因为它们直接决定结论方向。

### 3.1 第一轮：入口定位（上一版文档）

定位到 ORVIBO 云 TCP 回调 `0x188B4`（帧格式 `hd` + 大端长度 + 密钥模式 + CRC32 + 32 字节会话 + AES 正文 JSON）、AndLink MQTT 回调 `0x15510`、共同下载器 `0x19C14` → `0x5E838`、应用分区表 `0xE3B84`。这一轮只到“链路存在”，bootloader 侧空白。

### 3.2 纠错 1：bootloader 是 ARM，不是 Thumb

现象：按 Thumb 解码 boot 时，OTA 字符串全部呈 `RT-Threa|[d OTA package` 这类每 32 字节被打断的乱码，字符串引用几乎全丢（Thumb 43 处 vs ARM 67 处）。

原因：boot 分区同样是 32+2 分块编码，Thumb 解出来的只是“被 CRC 打断的数据”。改用 **ARM** 解码并补上 `add rD,pc,#imm` 引用解析后，`rt_ota_init`(0x9F28)、`rt_ota_check_upgrade`(0x99E8)、`rt_ota_upgrade`(0xA2B8)、`rt_ota_part_fw_verify`(0xA134)、`part_fw_hash_verify`(0x9344) 才各归其位。

注意：`0x9B74`（自定义校验）**反而是 Thumb**——它由 ARM 侧的 `blx` 调入，模式随 `blx` 切换，两种模式都要试。

### 3.3 纠错 2：尾块在分区末尾，不紧邻载荷

最初假设“载荷块后面紧跟 96 字节尾块”，于是在 `0xF98E0` 处块 CRC 断言失败。

实测：块 CRC 从 `0x11000` 连续有效到 `0x10A8E0`，之后是 0xFF 填充；尾块在分区**末尾** `0x131F9A`–`0x132000`。分区镜像 = `[载荷块][0xFF 填充][尾块]`。

### 3.4 纠错 3：分区描述符字段偏移

boot 里的分区描述符从 `0xD400` 开始、步进 `0x40`：`01PEdownload` / `01PEapp` / `01PEbootloader`。本轮复核确认：`download` 为 `beken_onchip`，使用物理地址 `0x132000/0xAE000`；app / bootloader 为 `beken_onchip_crc`，使用逻辑地址。app 的 `start=0x10000, size=0x110000` 正确，按 32+2 编码转换后对应物理 `0x11000–0x132000`，不是字段读偏。三项描述符末尾 CRC32 全部成立。应用 SDK 表中重叠的 `size=0x143000` 不能用于 boot 校验或制作镜像。

### 3.5 纠错 4：算法位定义（会得出相反结论）

最初假设“低位=压缩器、高位=加密”，按此假设 `0x102` 会被读成 FastLZ+AES 而判“不支持”。逐分支解析 `fw_algo_support_check`（0xA504 起）的 PC 相对取址后，真实集合是：

| 值 | 含义 |
| --- | --- |
| `0x000` | NONE |
| `0x002` | AES256 |
| `0x100` | GZip |
| `0x102` | **GZip + AES256** |
| `0x200` / `0x202` | QuickLZ / QuickLZ + AES256 |
| `0x300` / `0x302` | FastLZ 系列 → 本版本直接拒绝（`Not supported FastLZ compress firmware`） |
| 低位 `0x1` | XOR → 拒绝（`Not supported XOR firmware`） |

即 bit1=AES256、bit8=GZip、bit9=QuickLZ、bit8|bit9=FastLZ。社区 `.rbl` 实测算法字段 `0x0102`，正是 GZip+AES256。

### 3.6 纠错 5：自定义校验的参数语义

按 Thumb 读 `0x9B74` 时出现两个反常：`crc16(buf,0x20)` 却去读 `buf[0x20]`（越界）、以及“CRC 匹配反而报错”。

从拷贝路径恢复调用约定后自洽：

```text
rt_ota_custom_verify(part, offset, buf, len)     // 调用点 0x8AE4 / 0x72C4
  offset = 目标偏移（< 0x400 才检查），buf = 已解密解压后的纯固件块
  函数内部先 r4 = buf + 0x20，后面读到的“值”是载荷第 0x20 字节起的 32 位字
```

于是：`crc16(载荷[0:32]) == BE16(载荷[32:34])` 说明“载荷已按 32+2 加帧”，报 `Please use none crc image to build rbl file!` 并拒绝；否则校验载荷第 0x20 字节起的 32 位值是否落在 app 分区内，不满足则报 `The application must link to the flash 64KB location!`。

---

## 4. 发现

### 4.1 原厂 bootloader 的 OTA 实现

| 项 | 事实 | 位置 |
| --- | --- | --- |
| 库 | RT-Thread OTA 0.2.4（`RT-Thread OTA package(V%s)`） | 0x9F28 |
| 分区 | `download` 物理 0x132000/0xAE000（`beken_onchip`）；app / bootloader 为逻辑 CRC 地址 | 0xD400 |
| 包格式 | 96 字节头：`RBL\0`、包版本、算法、时间戳、名 16、版本 24、SN 24、载荷 CRC32、FNV-1a、raw 长度、包长度、头 CRC32 | 0xA134 / 0x9344 |
| 完整性 | 逐 32 字节 FNV-1a（基准 0x811C9DC5、乘数 0x01000193）与头内 hash 比较；另有载荷 CRC32 | 0x9344 |
| 非对称签名 | 未检出 RSA/ECDSA/SHA-2/signature 相关字符串；不能据此证明没有签名验证 | 全扇区字符串检索，完整校验路径待验证 |
| 对称密钥 | BK7231 默认 AES key/IV 常量存在，与上游 boot 逐字节一致（即仓库 `verify_firmware.py` 已用的默认值，此处不抄字节） | 0xC7C 起 |
| 升级判定 | `memcmp(下载区头, 应用尾块头, 96) != 0` 即升级，无版本序 | 0x99E8 |
| bootloader 保护 | `Can not upgrade bootloader partition!` | 0x92B0 |
| 自定义校验 | 见 3.6；原厂比上游多两条分支 | 0x9B74 |

哈希约定得到实测印证：`fnv1a(原厂应用前 0xEADFF 字节) == 应用尾块 hash 0xD88CB993`，与社区打包脚本 `assert fnv1a(raw)==fnv` 一致。原厂应用尾块 `name=app`、`version=2M.1220`、raw 0xEADFF；boot 尾块 `name=bootloader`、raw 0xD4C0。

### 4.2 应用侧下载不校验内容

`0x5E838`：两个 1024 字节缓冲、端口 80 明文 GET、超时 20000 ms、返回 0 即 `0x5D824` 重启。落盘路径 `0x28224` 选分区 2（`ota` 0x132000）、`0x28148` 按 4 KiB 扇区擦写并回读比较、`0x28398`/`0x28328` 分块重试。全程**没有** RBL 头／名称／版本／CRC／签名检查，字节流原样落盘。

### 4.3 触发通道

- ORVIBO 云 TCP `homemate.orvibo.com:10001`（明文 TCP + 应用层 AES，共享密钥在固件内），回调 `0x188B4`，`cmd=0x4C` + `type=4` + `url` → `0x19C14`；
- AndLink MQTT 回调 `0x15510`，`function=file` + `fileUrl` → `0x19C14`；
- 串口 CLI `http_ota <url>`（`0x3EDD8`，仅串口）；
- 本地 UDP 命令表只有 `outletStatus`/`setTime`/`SystemControl`/`pwCutMemory`/`powerConsumptionClear`，**没有** OTA 命令。

### 4.4 信任模型

已确认 CRC32 / FNV-1a 完整性检查与固定密钥对称加密。尚未完整验证网络登录、会话和启动器安装链，不能据此断言没有其它来源认证，也不能把共享密钥加密本身视为来源认证。

### 4.5 一处可致控制流的栈溢出

`0x15208`：`sscanf(item->valuestring, "%[^:]%*[://]%[^:]%*[:]%ud", sp+8, …)`，`%[^:]` 无字段宽度，目的地 `0x1525A` 为 `add r2, sp, #8`；该帧仅 32 字节（8 个 push 字），越界写入覆盖保存的 r4/r5/r6 与 LR。入口 `0x17950`（HTTP 响应解析）→ `0x17A54` → `0x15208`。静态可致劫持，未动态验证。

### 4.6 前提更正：设备当前不是原厂 bootloader

首次拆机写入的是自 0 起 1220464 字节 QIO 镜像（0 到 0x12A000），`0x0`–`0x11000` 是 OpenBeken 上游 bootloader（尾块版本 `1.00`、raw 59932）。原厂 bootloader（`2M.1220`、raw 54464）从未参与任何 OTA。**因此此前“社区 OTA 成功”只证明上游 bootloader 接受社区包。**

---

## 5. 验证路径

### 5.1 已完成的离线验证（可复现）

| 验证 | 手段 | 结果 |
| --- | --- | --- |
| 应用解码与已知 SHA 一致 | `BekenCodeCipher((0,0,0,0))` + 去块 CRC | 一致 |
| 两个尾块 magic／名称／算法／头 CRC32 | `footer.py`、`stock_ota_inspect.py` | 通过 |
| FNV-1a 约定（原厂镜像 vs 社区打包） | `fnv1a(app[:raw]) == 尾块 hash` | 成立 |
| bootloader 签名名称检索 | 全扇区字符串检索 | 未发现；不能证明没有签名验证代码 |
| 算法位接受集合 | 逐分支 PC 相对解析 | `0x102`=GZip+AES256 接受 |
| 自定义校验对待装入载荷 | unicorn 仿真 `stock_ota_emulate.py` | 本次纯应用接受；已加帧及链接字越界等错误输入拒绝 |
| 分区容量 | 尺寸比对 | 应用 654032 B、包 465728 B，均放得下 |

仿真用例结果：

| 用例 | 结果 |
| --- | --- |
| 社区应用纯固件首块（`part=download, offset=0`） | 接受（返回 0，无错误输出） |
| offset ≥ 0x400 | 接受（跳过） |
| `part=app` 自检 | 接受（检查不适用） |
| 载荷已按 32+2 加 CRC16 帧 | 拒绝：`Please use none crc image to build rbl file!` |
| 当前分区名非 app | 跳过：`The current part name: … does not need to custom verity.` |

### 5.2 尚未完成的验证（按建议顺序）

1. **端到端离线仿真**：把 `decrypt & decompress`（0x748C / 0x880C）与写应用分区路径（0x8184 写、footer 重建）跑通，核对安装后应用分区内容与 footer 自洽；完整仿真仍不能代替真实设备安装与启动。
2. **协议侧**：离线做帧 AES/CRC 向量，隔离调用 `0x188B4` 确认 `cmd=0x4C` 分支的登录／会话前置条件（不输出密钥）。
3. **实机（唯一能宣布可用的验证）**：具备串口恢复的机器 → 恢复原厂固件 → 让原厂 OTA 入口把 `.rbl` 落到 `0x132000`（或直接写该分区）→ 重启观察。判据：串口日志无 `Custom verify…`／`App verify failed!`／`Not supported this compression…`；启动版本变为社区版本；身份／射频／配置保留。
4. **溢出**（如需）：只在离线仿真或已确认的测试设备上进行，不放宽“首次安装仅拆机”的公开说明。

### 5.3 失败时的日志定位表

| bootloader 输出 | 含义 | 相关位置 |
| --- | --- | --- |
| `Not supported this compression/encryption algorithm` | 算法字段不在接受集合 | 0xA504 起 |
| `Not supported FastLZ compress firmware` | 用了 0x300/0x302 | 0xA550 |
| `Custom verify RAW firmware failed!` | 载荷被 0x9B74 拒绝（多半是喂了已加帧镜像） | 0x93B8 |
| `The application must link to the flash 64KB location!` | 载荷第 0x20 起的字不在 app 分区内 | 0x9D20 |
| `Verify firmware hash/CRC32 … failed` | FNV／CRC 与头不符 | 0xBAE8/0xBB84 |
| `The partition (%s) has no space …` | raw+0x60 超过 app 分区 | 0xA414 |
| `Can not upgrade bootloader partition!` | 包名写成了 bootloader | 0x92B0 |
| `OTA upgrade failed! Need to recovery factory firmware.` | 安装／写入失败 | 0x926C |

---

## 6. 复现命令

```sh
source ~/.cache/phicomm/env.sh gcc5
R=$S30_RUN/..

# 静态事实复查（应用 + 原厂 flash）
python3 BL2028N/research/stock_ota_inspect.py \\
  --app   "$S30_RUN/hardware-live-meter/factory-app-decoded.bin" \\
  --flash "$R/original-flash-read1.bin" \\
  --output /path/to/private/stock-ota-static.json

# 原厂 bootloader 自定义校验仿真
python3 BL2028N/research/stock_ota_emulate.py \\
  --boot "$R/original-boot-zero-decoded.bin" \\
  --app  "$S30_RUN/build-run-shared-services/firmware/BL2028N_app_<version>.bin"
```

私有辅助脚本（索引器、栈帧、参数恢复、字面量池扫描、尾块解析、仿真原型）在 `$S30_RUN/stock-ota-analysis/round2/`，清单见该目录的 `README-round2.md`。

---

## 7. 产物清单

| 文件 | 说明 |
| --- | --- |
| [stock-ota.md](stock-ota.md) | 结论与限制（对外口径） |
| [stock-ota-exploration-log.md](stock-ota-exploration-log.md) | 本文：过程、纠错、发现、验证路径 |
| [stock_ota_inspect.py](stock_ota_inspect.py) | 静态事实复查工具（`--app`，可选 `--flash`） |
| [stock_ota_emulate.py](stock_ota_emulate.py) | 自定义校验 unicorn 仿真工具 |
| [evidence/stock-ota-static.json](evidence/stock-ota-static.json) | 复查工具输出 |
| 私有 `round2/` | 索引器与分析脚本、10 份关键函数反汇编、仿真结果 JSON |

---

## 8. 限制

- 原厂 bootloader 从未实机跑过社区包；`decrypt & decompress` 与写应用分区未端到端仿真。
- `0x9EE4`（“当前分区名”辅助）的语义按仿真建模，未逐条追到底；它只影响“跳过”，不影响“拒绝”。
- 自定义校验“CRC 匹配即拒绝”的语义来自代码与仿真，缺少厂商文档印证。
- 应用侧触发需要控制下发方；本记录不改变“首次安装仅提供拆机刷机”的公开说明。
- 私有数据留在私有目录，不进 Git；不输出密钥或设备身份。

## 2026-10-30 本轮复核与统一布局结论

本轮通过共享 Python 重新运行静态核对，加入三项 boot 描述符的真实字段与 CRC32 校验。修复仿真器误用应用 SDK 重叠长度 `0x143000`，改从 boot app 描述符读取 `0x10000/0x110000`，并固定 boot SHA、验证仿真确实返回。原厂 `2M.1220` 与社区 `1.0.0-rc.3-dc3a36` 各 13 个正常、错误和边界输入通过。辅助分区名查询仍为 stub，完整安装未执行。

原厂应用已私有重打包为 649504 B 的 GZip+AES256 RBL，独立 CRC/FNV/解密/解压后与原厂 Flash 中的纯应用逐字节相同。新社区 RBL 为 476320 B，两者都能放入原厂 696 KiB 下载区。当前社区保留默认 LittleFS 后仅余 632 KiB，原厂恢复包按扇区取整后多一个 4 KiB 扇区。直接去掉保护，或只改 OTA 起点，都将破坏旧 LittleFS。

新增只读 [stock_partition_layout.py](stock_partition_layout.py)，用真实整片备份回放数据迁移与两种包的暂存；候选 CFG `0x1E2000–0x1E3000`、LittleFS `0x1F0000–0x1F8000` 避开已识别的两套 EasyFlash、RF、NET info 与 OTA 区。原厂尾部其它生命周期的写入范围仍需核对。详细证据、推荐布局、单应用槽限制和双向实机步骤见[统一分区研究](stock-partition-layout.md)。

本轮仅完成离线研究，没有刷入原厂启动器或统一布局固件，原厂网络入口、完整安装和双向免拆切换均未实机验证。文中历史的“无签名材料”仅能改述为“未检出相关字符串”，不能当作无签名验证的证明；局部自定义校验通过也不代表所有安装阻塞已清除。

## 2026-10-30 目标收敛：只要求原厂 → 社区免拆安装

用户明确不要求刷回原厂。执行范围改为打通原厂网络 OTA 入口、由原厂启动器安装社区应用、验证社区功能和后续社区 OTA；原厂恢复包与双向切换不再阻塞本目标。首次安装允许重新初始化业务配置并重新配网，保留工厂 MAC/RF。保留原厂启动器时，社区后续 OTA、应用尾块与配置地址仍须配套适配。当前仍缺网络入口实测和完整安装启动证据；串口或调试器调用不能记为免拆成功。详见[当前单向目标](stock-ota.md#当前目标原厂单向免拆安装社区固件)。

## 2026-10-30 实机：原厂网络 OTA 与后续社区 OTA 均通过

1. 在恢复原厂前双读当前社区 Flash 并校验一致；软件重启后原始 BootROM 握手成功。恢复本机原厂完整 2 MiB，独立回读与已固定备份逐字节一致，保留工厂 RF/MAC。
2. 通过正常 CoAP `/qlink/netinfo` 配置测试 CGW。原厂只接受 `https://` 引导地址；HTTPS 响应必须保持连接，才能继续接收 boot/dmReport。明文 MQTT 分支反复连接、未发送 CONNECT；分析发现连接函数与调用方的成功返回值不一致，改用原厂正常 `ssl://` 分支后，TLS MQTT 连接和订阅成功。
3. 向原厂实际 MQTT client ID 的下行主题发送正常 `function=file`、`fileMode=0`、`fileUrl` 命令。08:38:33 UTC，设备 HTTP GET 完整下载 476256 B；原厂 bootloader 输出 `2M.1220 -> 1.0.0-rc.3-dc3a36-dirty`、写入 100%，启动社区应用。升级阶段没有串口 OTA 命令、直接写下载区或调试器调用；没有利用 URL 解析溢出，也没有做 DNS 重定向。
4. 社区默认配置启动后，使用 OpenWrt 临时 STA 接口连接产品热点，通过正常 MessagePack `config.set` 配网，自动回到原局域网；临时接口、主机路由及进程完成后清理，不修改持久网络配置。
5. Chromium 和 WebKit 各 72 个真实 API 请求均通过；各 3 轮在 1 秒内完成 3 次标签切换，零空白帧、未保存内容保留，空密码刷新不要求登录。
6. 同一源码重新构建/打包，通过社区正常 HTTP `ota.start` 完成第二次下载和原厂启动器安装，约 49.6 秒恢复 HTTP，Wi-Fi 等设置保留。该次包头及内容与首次不同，版本名称相同，不算跨版本迁移验证。
7. 最后只读回读完整 Flash，并用设备 CRC 独立复核。原厂启动区与 RF/MAC 分区不变，实际纯应用及 `0x131F9A` 尾块与第二次 OTA 包完全一致；重启后 HTTP 恢复。两次构建的输入、固件、SHA-256、串口/网络原始记录及浏览器录像均已保留。

当前实测结论及制品见[实机记录](stock-ota-live-test.md)和[结构化证据](evidence/stock-network-ota-live.json)。此前双向布局回放仍属于离线证据；未验证刷回原厂、其它原厂版本、跨版本配置迁移或安装中断电。
