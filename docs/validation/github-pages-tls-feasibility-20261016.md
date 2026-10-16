# github.io OTA 小型 TLS 本地验证（2026-10-16）

> 本文记录实现前的实验/设计阶段。实际整机实现和设备验证见 [实现报告](ota-tls-implementation-20261016.md)。

结论：**保留 OTA 签名校验，仅跳过 TLS 证书校验**，已用 2 KiB TLS 记录缓冲完成
TC1、M1、A1 的真实 github.io 签名 OTA 下载、验签及模拟 Flash 读回检查。
TLS 部分相比首轮实验，固定 RAM 降低 69%，代码降低 32%。
这份报告替代首轮“不验证 OTA 签名”的实验结论。
生产设备 OTA 代码未修改；目前仍属于本地资源与传输验证。

## 实现和缩减方式

`tools/tls_probe/profile.c` 使用 BearSSL 0.6 的单一 TLS 1.2 套件
`TLS_RSA_WITH_AES_128_GCM_SHA256`（0x009c），RSA i32、小型 AES、GCM、SNI、半双工记录缓冲。
当前 github.io 端点实际接受此套件。避免额外引入 TLS ECDHE P-256 实现；
**OTA 的 P-256 ECDSA 验签仍由现有 micro-ecc 执行**。
只解码叶证书取得 RSA 公钥，不验证 CA、域名、有效期和证书签名。
TLS 记录认证及握手 Finished 检查保留；禁止连接重协商。
RSA/SHA-256 的 signature_algorithms 扩展保留，否则现代 OpenSSL 测试服务器拒绝握手。

`download.c` 是可交叉编译的实际分段下载逻辑，不依赖 POSIX 或 printf/scanf：

- 使用 `Range: bytes=start-end`，每次最多 1024 字节，复用 HTTPS keep-alive 连接。
- 严格检查 HTTP 206、Content-Range 起止位置及总长、Content-Length；拒绝压缩、分块及重复长度。
- 服务器显式关闭连接时重连，继续下一个区间；不重新擦除已写入内容。
- 逐块调用原有 `signed_ota_begin/write/finish`，处理 192 字节签名头。
- 先验证签名和型号，再执行模拟擦除；完成后从模拟 Flash 重新读取正文并验证签名头中的 SHA-256。
- 不接受无签名 raw 包；不接受 Range 被忽略后的 200 响应；传输失败即终止，不自动放大缓冲。

`client.c` 仅提供主机 DNS/socket、熵源、时钟、文件型 Flash 适配。
不存在将整个镜像读入客户端 RAM 的操作。模拟 Flash 文件只保存签名包的正文。

首轮完整 GET 在 2/4/8 KiB 缓冲下触发 `BR_ERR_TOO_LARGE`；
这限制的是完整响应模式。Range 把每个应用响应限制为小包，使本次 2 KiB 缓冲路径可用。
[BearSSL 文档](https://bearssl.org/api1.html)说明初次握手未加密记录可流式处理，
而加密记录需要完整接收后验证；因此证书链的总长度不必等于接收缓冲长度。
Range 并不是 TLS 层最大记录大小的协商保证：若站点返回过大的响应头或改变响应行为，客户端会报错终止。

## 三型号真实下载

测试端点 `https://phicomm-io.github.io/releases/v0.9.5/`；全部使用 `.ota.bin.signed`。
最终核心实现的一轮结果：

| 型号 | 签名包字节 | Range 请求数 | TLS 连接数 | 主机总耗时 | 验签 + 模拟 Flash 读回 |
| --- | ---: | ---: | ---: | ---: | --- |
| TC1 | 617704 | 604 | 1 | 36.841 s | PASS |
| M1 | 626264 | 612 | 1 | 35.515 s | PASS |
| A1 | 647856 | 633 | 1 | 39.082 s | PASS |

每次连接首个请求发送完成约 0.174 秒（包含主机连接与握手），没有每段重新握手。
模拟 Flash 正文还与发布目录的 raw SHA-256 一致；该比对是额外实验检查，不替代签名验证。
Range 带来约 600 次往返，比首轮完整 GET 慢。以上主机时间不是 MCU 握手、验签或写 Flash 时间。

## ARM 实测资源

工具链：设备现用 ARM GCC 5.4.1，Cortex-M4/Thumb/hard-float，`-Os`，
函数/数据段回收，newlib nano，无 LTO。
使用原有 `common/ota/signed_ota.c`、`crypto.c`、生产公钥 `keyring.h`，
以及 MiCO SDK 的 `SHAUtils/sha224-256.c`。
SDK 提交 `6c465211d3ff8797cd835e400ec54a06530dd476`，SHAUtils 无本地修改。
以下是独立 ARM 链接结果，不是主机 RSS，也不是最终三款整机链接增量。

| TLS 同口径比较 | 首轮 ECDHE + 完整 GET | 当前 RSA + Range | 变化 |
| --- | ---: | ---: | ---: |
| TLS text/rodata | 33781 B | 22852 B | -32.4% |
| TLS 上下文 + 记录缓冲，含对齐 | 21264 B | 6600 B | -69.0% |
| 记录缓冲 | 16709 B | 2048 B | -87.7% |

当前 ARM TLS 上下文仍为 4552 字节：client 3408、叶证书解码 1120、stream I/O 20，含对齐。
完整链路的实测如下，Flash = text/rodata + data，固定 RAM = data + bss：

| 独立链接对象 | Flash | 固定 RAM |
| --- | ---: | ---: |
| 原有 OTA 验签与读回核心 | 5538 B | 268 B |
| 小型 TLS 核心 | 22852 B | 6600 B |
| TLS + OTA 验签/读回核心 | 28304 B | 6872 B |
| **分段 HTTP 下载 + TLS + OTA 验签/读回** | **30514 B（29.8 KiB）** | **6904 B（6.74 KiB）** |

完整链路相对原有 OTA 验签核心的独立链接差额：**24976 B Flash、6636 B 固定 RAM**。
实际固件可复用已有 libc/OTA 符号，也要增加 MiCO 网络、熵源、线程适配；
最终差额须以整机链接确认。链接没有 TLS 内部 malloc/calloc；TLS 上下文和缓冲由调用方提供。
测量入口只替换硬件回调，保留了实际 `download.c`、解析、验签及读回代码。

### 栈与总预算

沿用现有 A1/M1 OTA 线程的 **6144 字节**作为候选栈预算，则本实验工作区约为：

`6904 + 6144 = 13048 B ≈ 12.74 KiB RAM`

这个预算已把请求/响应行 640 B、流式正文 512 B 和临时局部变量计入线程栈，不能再次相加。
SDK socket/TCP、RTOS 管理结构、分配器开销及并行业务不在其中。

GCC `.su` 实测单帧：下载核心 1264 B、TLS 握手解释器 608 B、RSA public 2104 B、
OTA write/验签入口 184 B、micro-ecc verify 536 B、OTA finish 688 B。
RSA 握手路径需要叠加下载核心、I/O、握手、RSA 和模运算；不能把最大单帧当成最大调用链。
6 KiB 是待设备水位验证的预算，尚非完整最坏调用链证明。
TLS 握手和 OTA 验签依次执行，不能机械地把两条路径的栈峰值相加。

## 校验及错误处理证据

`test_transport.py` 使用真实 TLS 服务端、实际生产签名 OTA 和原有 C 验证器。
服务端证书为自签名且域名不匹配，成功案例直接证明证书校验被省略而 OTA 校验保留。
6 个测试方法、14 个场景均通过；相同场景在 ASan + UBSan 下通过。

- 合法签名包：验签、分段写入、读回摘要通过。
- TLS/HTTP 任意分片：通过。
- 首段后关闭连接：新建第二条连接继续，仅擦除一次。
- 签名篡改、raw 包、错误型号：均在首次擦除之前拒绝。
- 正文篡改：签名头通过，但最终模拟 Flash 读回摘要拒绝。
- Range 被忽略、错误起点/长度、跨响应总长改变、分块编码、重复 Content-Length、截断正文：均拒绝。

原有 `tests/ota/test_signed_ota.py` 使用 SDK SHA 实现运行，7 个测试全部通过，
包括签名头各字节篡改、错误密钥、Flash 故障和中断。

## 复现

依赖只存于被忽略的 `build/`，生产固件构建不自动引入该实验。
BearSSL 0.6 归档 SHA-256：
`6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`。

```sh
export OTA_SHA_DIR=/path/to/mico-os/MiCO/security/SHAUtils
export ARM_PREFIX=/path/to/bin/arm-none-eabi-
tools/tls_probe/build.sh
python3 tools/tls_probe/run.py
python3 tools/tls_probe/test_transport.py
python3 tests/ota/test_signed_ota.py
```

不测 ARM 时可省略 `ARM_PREFIX`。运行目录下的 `build/tls-probe/` 保存：

- `results-signed.json`、`run-signed.log`：三型号下载、验签和正文一致性结果。
- `transport-tests.log`、`transport-asan.log`：本地真实 TLS 错误场景。
- `build-small.log`、`footprint-{full,combined,tls,ota}.{elf,map}`、`arm-sizes.txt`：ARM 资源。
- `download.su`、`signed_ota.su`、`crypto.su`、BearSSL ARM `.su`：单函数栈帧。

## 验证边界

已实现更小的本地候选并量化资源，尚未刷设备、完成三款整机链接或测量设备 heap/栈水位。
模拟 Flash 未包含真实擦除/写入耗时、原生镜像 ABI/CRC/内核兼容性检查或激活；
正式接入应继续执行现有产品 OTA finish 流程，不能用实验 finish 替代它。
A1/M1 应用 Flash 256 KiB、应用 SRAM `60K-16` 不等于运行时可用 heap。

当前候选依赖站点继续支持 RSA 密钥交换及 Range/keep-alive，以上是本次在线实测结果。
设备阶段还需证明约 12.74 KiB 工作预算之外的网络/业务余量、超时释放、熵源、watchdog 和最终分区余量。

## 后续：版本清单与 release log

同日扩展了通用流式 sink，固件仍使用原验签路径。新 full 独立链接 Flash 30598 B（增加 84 B）、固定 RAM 6904 B 不变。资源表上方是本次扩展前的基线值。元数据与日志在线实测、回归及线程模式设计见 [OTA 服务模式设计](ota-service-mode-design-20261016.md)。
