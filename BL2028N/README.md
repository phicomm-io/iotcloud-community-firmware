# ORVIBO S30-V1B / BL2028N

S30-V1B 单路计量插座，模块 BL2028N、芯片 BK7231N、计量 HLW8032。运行框架仅为 OpenBeken 和 BK7231N SDK/HAL/RTOS；公共协议与前端直接移植自 `phicomm-messagepack-lab`，不引入 MiCO、Arduino 或 ESPHome 运行时。对外项目名 `iotcloud`、协议名 `openiot`，命令封装为 TMCP v2。

用户说明：[拆机串口刷机](../docs/public/s30-flash.md) · [功能与操作](../docs/public/s30-guide.md)。首次安装暂时只提供拆机刷机，VTX/VRX 与插头面 PCB 右上方外侧的电源触点接线见刷机说明。

开发状态与接续入口：[工作记录](WORK_STATUS.md) · [原厂 OTA 静态分析](research/stock-ota.md)。最新候选固件已构建，但最终实机 OTA 遇到超时后仍停留在升级状态的问题，尚未通过最终验收，未合入 `main`。

本版修复 SDK 读取错误工厂分区导致 MAC 尾部全零的问题，并接入实际公共 HTTPD、MQTT 服务和完整公共前端。2026-10-30 已在 S30-V1B 上验证工厂身份、DHCP hostname、公共网页、HTTP/MQTT/UDP、空密码/有密码鉴权及此前版本的 URL OTA 自动重启上线；此前覆盖的错误下载可恢复服务。用户已确认短按、红灯、输出开启时蓝灯常亮、长按恢复以及每秒一次、松手恢复的计时灯语。修正计量串口为 UART2 后，已在原电源及真实负载下连续采样验证电压、电流、功率与累计电量，30 次采样均有效、期间坏帧数未增长；默认系数尚未经过标准仪表校准，不声明计量精度。

## 硬件与身份

| 功能 | 引脚 / 参数 |
| --- | --- |
| 按键 | P6，低电平按下，上拉，40 ms 消抖 |
| 蓝灯 | P7，高电平亮，表示插座输出通断 |
| 红灯 | P8，高电平亮，表示网络异常 |
| 继电器 | P9，高电平吸合，默认上电关闭，可配置 |
| HLW8032 TX → RX | UART2 / P1，4800 baud、8E1；原厂固件计量接收使用 UART2 |
| 拆机刷机 / 日志 | UART1，VRX / P10、VTX / P11；运行日志避开计量 UART2 |

按键、灯及继电器编号依据 [Elektroda 讨论](https://www.elektroda.com/rtvforum/topic4179248.html)；整机型号按用户确认使用 S30-V1B。GPIO 极性已按实物反馈修正。计量串口依据本机原厂备份：`0x26d2c` 注册 UART2 回调，`0x2759c` 从 UART2 读取，`0x27524` 校验 HLW8032 的 24 字节、`0x5a` 标记及校验和（解码后的应用逻辑地址），并已通过实机收帧验证。此前 UART1/P10 的默认假设不适用于本板。状态中的 `received_bytes`、`frames`、`bad_frames` 分别记录收到的字节、校验通过的帧和失配窗口，用于区分无串口输入与帧格式错误。

本板原厂备份中的 RF/TLV 分区在 `0x1e0000`，SDK 默认 `0x1d0000` 不适用。构建覆盖层修正 RF 分区，并由 `ph_identity.c` 校验 TLV 边界和有效单播 MAC。每次加载配置后更新原生 CFG/Wi-Fi MAC，因此可修复旧版本保存的默认地址。固件不包含任何单台设备的硬编码 MAC，也不写 EFuse。

配网热点为 `S30M_V1B_` 加原厂 MAC 后六位大写十六进制；默认 IP/网关 `192.168.0.1/24`，热点与管理密码初始为空。热点前缀保留用户指定拼写，整机型号始终为 S30-V1B。

DHCP hostname 为 `iotcloud-s30-v1b-` 加原厂 MAC 后六位小写十六进制，由原厂身份生成，不使用旧配置中的 OpenBK 长短名称；尚未取得有效工厂地址时不生成带默认 MAC 的名称，也不启动无效名称的热点。该格式遵守 DHCP 主机名字符约束，AP 则保留产品前缀与大写后缀。

RF 校准沿用 BK7231N SDK：启动校准、读取原厂发射功率表、温度及晶振等参数。实机诊断日志已确认校准完成、Flash 功率表及晶振参数读取成功；串口刷写后核对原厂 RF 扇区逐字节保留。没有通过仪器验证射频功率、频偏或接收灵敏度，不把正常联网当作射频精度验收。

### 指示灯

| 状态 | 蓝灯 | 红灯 |
| --- | --- | --- |
| 已配网、Wi-Fi 正常 | 输出开启常亮，关闭熄灭 | 正常熄灭；配置了 MQTT 且未就绪时亮灭各 1 秒 |
| 已配网、Wi-Fi 断开 | 继续表示输出通断 | 亮灭各 150 ms |
| 主/备用网络均未配置且未联网 | 与红灯交替，每色 500 ms | 与蓝灯交替 |
| 按住不足 1 秒 | 熄灭 | 熄灭 |
| 按住 1 秒至不足 5 秒 | 每满 1 秒亮约 160 ms | 熄灭 |
| 按住 5 秒至不足 10 秒 | 熄灭 | 每满 1 秒亮约 160 ms |
| 按住至少 10 秒 | 与红灯同步，每满 1 秒亮约 160 ms | 与蓝灯同步 |

重置提示优先于普通状态。关闭指示灯设置会关闭普通蓝灯和异常/配网灯效，但不屏蔽长按重置提示。没有配置 MQTT 时不显示 MQTT 故障；未实现额外 OTA 或计量故障灯效。红灯有效电平依据用户实物反馈修正，与蓝灯分别定义。

短按切换插座，童锁仅屏蔽短按。按住至少 5 秒、不足 10 秒后松开只清除主/备用 Wi-Fi；至少 10 秒后松开清除网络、产品配置、鉴权/配对与累计电量。长按计时与 TC1 的公共 `key_hold_feedback` 同步：从按满 1 秒起，每满一秒闪一次、亮约 160 ms，按下时刻与系统运行时间无关。第 1–4 次为蓝灯，第 5–9 次为红灯，第 10 次起红蓝同步，松手恢复普通灯效并执行相应动作。Wi-Fi 重置保留 MQTT、管理密码、配对、校准和累计值。短按后 1.5 秒内拒绝远程开关，避免立即覆盖本地动作。

## 公共工程同步

`protocol/lab-shared-source.json` 和 `web/shared/provenance.json` 固定来源提交、脏状态、原始及适配后 SHA-256；脏快照不能只用提交号代替。`protocol/lab-openbeken.patch`、`web/frontend-openbeken.patch` 保留适配差异，私有运行目录另外保存来源快照。

- HTTPD：直接编译公共 `httpd/core.c`、`parser.c`、`http.h`，OpenBeken 适配层提供 lwIP I/O、RTOS 线程、鉴权和产品命令。
- MQTT：直接编译公共 core/client/session/owner/queue/diagnostics，保留单 owner、队列准入、回执所有权及断线重连。Paho 来自项目固定 SDK 协议代码及同一组项目补丁，仅替换网络/计时/线程原语；不编译 MiCO 平台层。许可见 `protocol/paho/NOTICE.md`。
- UDP：提取公共 `feature_wire.inc` 的包解析器，复用 `local_auth` 与 typed codec，适配单路插座和 BK lwIP。
- 前端：使用完整公共 style、appearance、i18n、service、provision、ui、automation 与 wire codec，并适配公共 TC1 页面的一路插座、HLW8032 校准及能力声明。保持相同首页、设置导航、主题、语言和表单交互；不展示未实现的能力。全部页面离线内置。

## 二进制协议

HTTP、MQTT、UDP 业务仅接受 MessagePack / CBOR，JSON 仅用于 Home Assistant discovery/state。不提供旧 JSON REST、`/cm`、通用脚本/引脚配置、旧业务主题或自定义 settings API。

共同命令结构（以下是结构示意，实际发送二进制）：

```text
{request_id: "app-1", action: "outlet.set", params: {index: 0, on: true}}
→ {request_id: "app-1", code: "accepted", message: "accepted", data: {on: true}}
```

修改命令必须有 `request_id`，最多 64 字节，仅允许字母、数字及 `_-.`；只读命令可省略。最近 8 个修改命令保留幂等结果，跨 MP/CBOR 和参数键顺序不影响重试；同 ID 不同命令/参数返回 `conflict`。

| Action | params |
| --- | --- |
| `info.get` / `capabilities.get` / `config.get` / `power.get` | `{}` |
| `status.get` | `{}` 或 `{view: "live" / "full"}` |
| `outlet.set` | `{on: bool, index: 0}`，index 可省略 |
| `indicator.set` / `child_lock.set` | `{enabled: bool}` |
| `config.set` | `{section, values}`，见下表 |
| `wifi.scan` / `wifi.scan.get` | `{}`，异步启动/查询 |
| `system.reboot` | `{}` |
| `system.reset` | `{full: bool}`，false 仅重置 Wi-Fi |
| `ota.start` | `{url: "http://…/BL2028N_OTA.rbl"}`，URL 最多 127 字节 |

`config.set` 的 section/values 与公共前端保持一致：

| section | values |
| --- | --- |
| `device` | 可选 `name`、`power_on` |
| `socket` | `{index: 0, name}` |
| `report` | `{seconds: 1..3600}` |
| `meter` | 三项可选 `voltage_coefficient`、`current_coefficient`、`power_coefficient`，范围 `0.001..100` |
| `wifi` | `{ssid, password?}`；省略密码保留原值，空字符串连接开放网络，保存后重启 |
| `mqtt` | `{host, port, user, password?, protocol?: "tmcp"}`；省略密码保留，空字符串清除 |
| `auth` | `{password}`，空字符串清除，非空为 8–32 字节可打印 ASCII |

状态提供公共前端所需的 `name`、`device_id`、`version`、`socket_states`、`socket_names`、`child_lock`、`led_enabled`、`power_on`、`report_seconds`、网络/诊断及计量字段。`capabilities.get` 如实声明单路插座、二进制格式、计量能力、URL OTA，禁用上传 OTA、配置恢复/备份、在线升级和自动化入口。

### HTTPD

支持 `GET /api/v1/info`、`/status`、`/capabilities`、`/config` 和 `POST /api/v1/command`。请求 `Content-Type` 与响应 `Accept` 独立支持 `application/msgpack`、`application/cbor`；JSON 输入返回 415，JSON Accept 返回 406。最大请求 1200 字节、响应 4096 字节。

空管理密码直接访问网页和本地 API，不显示登录框、不分配会话。设置密码后网页使用项目 `admin` Basic 鉴权。API 另可用二进制 `POST /api/v1/auth/login` 的 `{password}` 获取 Bearer token，`GET /api/v1/auth/status` 查询状态，`POST /api/v1/auth/logout` 销毁会话。四个会话槽，空闲 30 分钟、最长 24 小时；正确密码的新登录可替换最久未使用槽。改密码撤销 HTTP/UDP 会话和持久配对；空密码不取消 UDP 配对或 MQTT broker/ACL。

公共 HTTPD 为四连接槽、一个请求/响应租约、12 KiB owner 栈；头部超时 5 秒，业务体空闲 2 秒/总计 5 秒，响应 10 秒。空闲浏览器预连接不会占住唯一业务租约。

### MQTT / Home Assistant

主题根 `phicomm/v2/<scope>/s30-v1b/<mac>`；无用户名 scope 为 `local`，否则为 `u_` 加用户名 UTF-8 字节的小写十六进制编码。

| suffix | 内容 |
| --- | --- |
| `command` → `result` | 非 retained MP/CBOR 命令及同格式结果 |
| `state` | retained MP 完整状态 |
| `availability` | retained `online`，异常断线遗嘱 `offline` |
| `ha/set` | HA 专用 `ON` / `OFF` 文本控制 |
| `ha/state` | HA 专用 JSON 状态 |

公共 MQTT session 发布 scope 下的二进制发现声明。HA 自动发现六个实体：一路插座、电压、电流、功率、累计电量、计量有效性。缺失或过期采样输出 null。业务命令拒绝 JSON 与 retained 控制；HA 文本控制只接受指定主题的 ON/OFF。修改 broker 账号时，命令回执仍发到收到命令时的主题，再按新配置重连。

当前 MQTT 仅 TCP；原生配置若启用 TLS 则拒绝连接，不降级。UDP owner、MQTT owner 各 16 KiB 栈。命令先预留最大回执缓冲区，再执行操作；编码后复制为实际长度并释放大块，避免 BK 分配器不会缩小 realloc 导致回执长期占用 4 KiB。

三项服务线程共预留 44 KiB 栈。此产品将原生连续六次低堆重启阈值从 25 kB 适配为 8 KiB，避免服务正常启动后的剩余堆触发循环重启；低于新阈值仍保留恢复保护。实机协议和 OTA 已验证；正常运行余量约 11–13 KiB，升级期间约 9.5 KiB，受连接及运行状态影响。

### UDP 11288

沿用公共 v2 包 `{v,src,dst,boot,id,op,token,body}`，允许公共 group/domain/kind 字符串元数据。`src` 为控制端 12 位小写 MAC，boot/id 为非零 u32。收发均使用 11288 端口。

`auth.pair` body 为 `{peer: src,password,scopes:7}`，返回持久 key；scopes 为 control=1、sensor=2、group=4 的非零组合，省略时为 7，仅 sensor 的配对不能控制插座，权限随 key 持久化。`auth.resume` 携带 `{peer: src,key}`，返回本次 token。后续 `command` 携带共同命令，响应 `result`。UDP 可读/控制，不授予配置和重启管理权限；另支持 `probe → pong`、`read → sample`、只读 `discovery → discovery.response`。广播仅允许发现。

会话检查递增 id 与 boot，控制端重启后 resume 轮换 token，保留四个退休 boot 防重放。重试业务命令时使用新包 id 和相同 request_id。

BK SDK 随机数驱动在初始化时启用生成器，并在读取之间保持运行，修复本板原生驱动重复产生相同启动标识的问题。实机多次重启的 boot 及连续八次登录 token 已检查不重复；这不代替随机源的密码学评估。

### OTA

支持公共网页中的 URL 升级，使用本设备构建的 `.rbl`。收到命令后保留回执发送宽限，等待公共 MQTT owner 发送回执并释放 socket/缓冲区，再开始下载；同时暂停 UDP、扫描和新的配置/控制操作，HTTP 保留两个连接槽供状态读取。失败时恢复服务，成功后保持维护状态直到自动重启。重启后的继电器遵循保存的上电策略。

HTTP 接收和 Flash 流缓冲区各为 512 字节，写入单位为 512 字节；每个 4 KiB 物理扇区只擦除一次。开始下载前至少保留 12 KiB 堆，升级期间继续使用正常的 8 KiB 低内存保护。实机验证了 MQTT 回执先送达、服务暂停、控制忙返回、三种失败下载恢复及随后完整升级重连。

已知缺陷：从 `0.9.27-299121-dirty` 升级到 `0.9.27-13e52e-dirty` 的最终测试中，下载约 120 秒后停在 32%，超过 360 秒仍未退出维护状态。现有 TCP 层超时测试没有覆盖 HTTP 客户端将零字节超时误作成功的完整路径；上述失败恢复结论仅适用于此前已测错误，不能覆盖这一缺陷。详细证据与修复入口见[工作记录](WORK_STATUS.md)。

当前未实现网页本地文件上传和数字签名验证，前端不显示相应入口。RBL 的 CRC/FNV 和 AES 打包不是数字签名。应用与 OTA 分区还有 Flash 余量，但签名验证需要另外实现流式校验、公钥和拒绝路径，并测量运行内存；不能把分区剩余空间当作已支持签名。保留原厂 bootloader 的现有升级格式。

## HLW8032 与存储

计量依据 [HLW8032 Rev 1.5 手册](https://raw.githubusercontent.com/Khaalidi/HLW8032/master/Docs/DS_HLW8032_EN_Rev1.5.pdf)，实现 24 字节帧、校验、状态/更新位、5 秒有效期及 17 位 PF 脉冲计数。缺失/校验错误/无效寄存器使用 null，不用假零。PF 计数处理回绕及异常跳变重建基线。

默认电压/电流/功率系数 `1.88 / 1 / 1.88` 仅为启动值，未校准。网页可分别设置，实际系数须标准负载核验。累计电量每 5 分钟、配置保存及正常重启前持久化，突然断电可能丢失最近 5 分钟。

产品配置用带 CRC 的 littlefs 临时文件、sync、原子 rename；显式迁移旧版布局，保留校准、配对、累计电量、童锁及上电策略。网络和管理密码保存在 OpenBeken 原生 CFG。

公共服务在原生启动延时结束后、HTTP 接纳连接前初始化，正常启动及 OpenBeken 安全模式均启动产品状态、按键/计量定时器和网络服务。安全模式保留恢复访问，继电器以关闭状态启动，但不改写已保存的上电策略。

## 构建与测试

先阅读工作区 AGENTS.md 和 `~/.cache/phicomm/POLICY.md`。使用共享 GCC 5.4.1 与 Python，不向工作区安装工具链。OpenBeken 固定 `1.18.319` / `7b754d6fc80b72115ece31fcf4527f011f90deef`，BK7231N SDK 固定 `07fd9e0e2da0aabaddab00be2a897589be235158`。

```sh
./BL2028N/build.sh --run-dir /path/to/lab/private/bl2028n-run
```

`--keep-build` 保留活跃构建，`--incremental` 更新覆盖层；默认成功后回收 SDK 副本。版本遵循 `tools/check_version.py` 的仓库标签、HEAD 前六位及必要的 `-dirty`；标签构建校验 HEAD 和干净状态。run 保存 compiler/build 日志、version、manifest、framework/SDK patch、ELF/map、校验结果及固件 SHA256SUMS。

| 文件 | 用途 |
| --- | --- |
| `BL2028N_BK7231N_QIO_*.bin` | 常规 N 密钥工厂镜像，含 bootloader，起点 0 |
| `BL2028N_BK7231M_QIO_*.bin` | 零密钥工厂镜像，含 bootloader，起点 0 |
| `BL2028N_*_UA_*.bin` | 对应密钥的 UART 应用镜像，起点 0x11000，保留 bootloader |
| `BL2028N_OTA_*.rbl` | OpenBeken 原生 URL OTA |
| `BL2028N_UG_*.bin` | 带 UG 头的升级工具格式 |
| `BL2028N_app_*.bin` | 未加密应用，仅分析，不直接刷写 |

串口前双读原固件并验证一致，再按原厂 bootloader 的实际密钥选择镜像，保留 RF、配置和 littlefs。BK7231M 文件名表示零密钥打包方式，SDK/目标仍是 BK7231N。本测试设备原厂为零密钥。禁止只看芯片名称选镜像。

构建自动验证块 CRC16、RBL CRC32/FNV、两种应用密钥独立解密、OTA AES/gzip 解包、UG 头、分区边界、必要已定义 ELF 符号和禁用框架/旧 JSON 符号。OTA 下载上限覆盖 littlefs 边界，超时 120 秒，拒绝非 200、坏头、超长或不完整包；bootloader 校验最终包。没有接入 MiCO 的签名 OTA。

```sh
source ~/.cache/phicomm/env.sh gcc5
PHICOMM_PLAYWRIGHT_MODULE=/path/to/existing/playwright PHICOMM_CHROMIUM=/path/to/existing/chrome \
python3 BL2028N/tests/run.py --run-dir /path/to/lab/private/tests --stock-backup /path/to/private/original.bin
```

主机测试在 ASan/UBSan 下直接编译产品与公共服务源，使用真实 TCP/Paho MQTT/UDP 和 Chromium，无 API mock；仅 HAL、RTOS、存储及 socket 平台绑定为 POSIX 替身。覆盖二进制协议、幂等/鉴权/重放、配置迁移、工厂 TLV、MQTT 回执/重连/retained、HTTP 预连接/分片、空密码重复刷新和完整前端表单/中英/手机布局，并保存页面录像和截图。

`test_startup.py` 编译实际原生启动函数，覆盖正常/安全模式及已配网/未配网四种路径的服务顺序；`test_http_timeout.py`、`test_ota_callback.py` 提取已覆盖的原生 OTA HTTP 客户端/回调测试真实套接字超时、错误和闪存边界；`test_ha_numbers.py` 使用实际 nanoprintf 测试 HA 数值。均接受 `--app-source <build/app> --run-dir <evidence>`。这些结果不能替代目标 RTOS、射频/联网、真实闪存、交流计量、继电器/按键及 OTA 重启验收。

启动排障可在构建时加 `--boot-trace`：使用现有编程串口 UART1 TX/P11 输出原生日志、启动步骤、线程创建结果和剩余堆，日志串口为 4800 baud、8E1；HLW8032 独立使用 UART2 RX/P1，UART1 RX/P10 不接收计量。构建清单记录此选项，校验器核对镜像中的诊断标识。正式验收必须关闭该选项并重新验证，不能用诊断版本代替最终固件。
