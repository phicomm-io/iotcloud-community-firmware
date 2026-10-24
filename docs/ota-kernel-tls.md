# OTA 内核 TLS 与密码学边界

A1、M1、TC1 的 OTA 下载统一调用 MiCO 内核的 `ssl_connect_sni`、`ssl_send`、`ssl_recv`、`ssl_pending`、`ssl_get_error` 和 `ssl_close`，不再编译应用侧 BearSSL TLS。ESP32 使用 ESP-IDF / mbedTLS。签名包格式、发布密钥及签名先于写入的检查不变；HTTPS 保留原有 SNI、无 CA 校验策略，发布者身份由 PHOTA001 签名验证。

## 超时、取消与所有权

- DNS、TCP、数据传输每次等待最多 200 ms 后重新检查取消和总截止时间。TCP 连接和单次 I/O 上限 10 秒，整个下载上限 180 秒。
- 内核的 SNI 握手 API 是同步调用。先设置 `SO_RCVTIMEO` / `SO_SNDTIMEO` 为 1000 ms（MiCO ABI 使用整数毫秒），约束当前已经阻塞的调用。另用一个 1024 字节栈的 RTOS 观察线程，每 20 ms 检查取消、总截止时间及 10 秒握手上限，到期通过 `fcntl` 切换 socket 为非阻塞，阻止内核开始下一次阻塞等待。单次读取超时无法限制持续慢速发送的服务器，因此两者都需要；握手退出允许当前 I/O 的超时余量。
- 观察线程不执行 shutdown、close 或释放 TLS。旧方案在 A1 的早期取消测试中复现 OTA 线程的内核网络 HardFault，故去掉跨线程 shutdown。下载线程等待观察线程结束后才进行自身的清理，并释放信号量和 transfer；内核自身关闭 fd 的路径另按下表处理。
- 握手结束后同时设置 socket 与 TLS 非阻塞模式。写操作循环消费实际发送字节；读操作接受任意正长度短读。WANT_READ / WANT_WRITE 分别等待相应方向，检查 TLS 已缓冲数据，避免已解密数据仍等待 socket。0 / 其它错误作为失败。
- 每个错误、取消、重连与退出共用清理路径。创建第二个信号量或观察线程失败也释放已创建的资源。服务器要求关闭连接时，下一个 Range 请求建立新会话。

两个已锁定的内核有不同的 socket 所有权。不能仅审计 SDK 的 `Lib_wolfSSL` 静态库来推断固件实际调用的内核：

| 设备 / 内核版本 | 实际 `ssl_close` | 下载器清理 |
| --- | --- | --- |
| A1、M1 / `3080B002.024` | 释放 TLS 会话、socket 及上下文 | 调用后清除 fd，避免重复关闭已被其它线程复用的描述符；握手失败的 -1 / -203 / -125 早期错误仍由调用者关闭 fd |
| TC1 / `30300002.015` | 释放 TLS 会话及上下文，保留 socket | `TC1/ota_fetch.c` 选择 `OTA_TLS_CLOSES_SOCKET=0`，成功或握手失败后均显式关闭原 socket |

二进制核对来自本项目打包的内核及 MOC 导出表。A1/M1 填充内核 SHA-256 为 `f22ad344286bb97755ab7ba2fb18e8768140c5383875098dfb0449d2184abc74`，TLS 表文件偏移 `0x6bd74`、`ssl_close` 地址 `0x08061273`。TC1 填充内核 SHA-256 为 `211e2a9d41f734300f42f8216538cf83f02c1f22ed434ecfe50d3f6aeb0e7047`，MOC 表偏移 `0x58558`、`ssl_close` 地址 `0x1f01c78d`。后续更换 SDK/内核时需重新核对此行为。

## LAN 所需算法

审计基于锁定 MiCO SDK 3.6.4、提交 `6c4652` 的 MOC 导出表（实际完整提交见 `common/mico-sdk.json`）。原工作区的 LAN 研究模块未加入常规构建；其依赖不能用普通固件的体积差推算。

| 算法 | 已见内核接口 | 处理方式 |
| --- | --- | --- |
| AES-128 单块 | `AesSetKeyDirect`、`AesEncryptDirect`、`AesDecryptDirect` | 可用于 LAN 的 CCM 单块运算，需按内核上下文 ABI 接入 |
| AES-CBC | `AesSetKey`、`AesCbcEncrypt`、`AesCbcDecrypt` | 直接导出；CBC 本身不能替代 CCM |
| SHA-256 / SHA-1 | MX1290 的扩展 `InitSha256` / `Sha256Update` / `Sha256Final` 等 | 扩展包装检查 `EXTRA_CRYPTO_FLAG`，缺失时返回 -1，不能假定所有已安装内核都支持 |
| HMAC | MX1290 扩展 `HmacSetKey` / `HmacUpdate` / `HmacFinal` | 同样需要检查扩展可用性；MW3xx 导出表没有同等扩展保证 |
| P-256 / ECDH / ECDSA | 未发现可直接调用的原语导出 | 固件验签仍需应用侧 micro-ecc，LAN 可保留自身所需的 ECC 链接；`ssl_set_ecc` 只控制 TLS 功能，不是 ECC 原语接口 |

对应源码：`include/mico_security.h`，`platform/MCU/{MX1290,MW3xx}/moc/moc_api.h`，以及 MX1290 的 `moc_api_sep.h`、`moc_adapter.c`。当前 OTA 签名的 micro-ecc、SDK SHAUtils 不依赖可选扩展；写请求去重也复用同一 SDK SHA-256。将来启用 LAN 应单独验证 AES/哈希 ABI、CCM 向量、ECDH/ECDSA 向量和完整链接映射；当前变更没有删除原工作区未提交的 LAN 实现。

## 证据与限制

`python3 tests/test_ota_fetch.py` 使用生产下载代码及真实 OpenSSL TLS 的 MiCO API 主机适配，开启 ASan/UBSan，并分别模拟上述两种 socket 所有权。覆盖短读（23 字节）、短写（17 字节）、三种 WANT 方向、SNI、每段重连、畸形响应、握手停滞、总超时和取消；同一进程连续重试，注入信号量/线程/TLS 分配失败，每轮检查会话、信号量、线程及文件描述符归零。停滞与持续慢速握手都测试超时和取消；任何应用侧 shutdown 调用都会触发断言。内核关闭 fd 后立即复用该编号，额外确认下载器不会再次关闭它。

本地构建及 CI 均锁定官方前端 `92f3b8a` 归档，最终发布构建以 CI 产物为准。主机适配不执行专有 MiCO 内核，不能替代 A1/M1/TC1 的堆、栈和实机取消测试。

2026-10-24 实机验证仍在进行：旧方案在 A1 复现 HardFault（OTA 线程、CFSR `0x00020000`、PC 为 0），已保留二进制诊断记录。切换非阻塞的测试版本通过连续六轮分片 TLS、取消及停滞服务器测试，再通过四轮早期取消，无重启且 socket 均关闭；持续慢速握手约 11 秒退出，去除调试接口后的候选仍需复测。M1 旧候选停在内核握手，需要断电恢复。TC1 首轮分片传输失败后失联，由此核对出上述 socket 所有权差异，修复也待恢复后实机复测。四台设备全部通过前不触发稳定版发布。
