# OTA 内核 TLS 与密码学边界

A1、M1、TC1 的 OTA 下载统一调用 MiCO 内核的 `ssl_connect_sni`、`ssl_send`、`ssl_recv`、`ssl_pending`、`ssl_get_error` 和 `ssl_close`，不再编译应用侧 BearSSL TLS。ESP32 使用 ESP-IDF / mbedTLS。签名包格式、发布密钥及签名先于写入的检查不变；HTTPS 保留原有 SNI、无 CA 校验策略，发布者身份由 PHOTA001 签名验证。

## 超时、取消与所有权

- DNS、TCP、数据传输每次等待最多 200 ms 后重新检查取消和总截止时间。TCP 连接和单次 I/O 上限 10 秒，整个下载上限 180 秒。
- 内核的握手 API 是同步调用。握手期间使用一个 1024 字节栈的 RTOS 观察线程，每 20 ms 检查取消、总截止时间及 10 秒握手上限，到期对当前 socket 调用 `shutdown(SHUT_RDWR)`，使阻塞的底层网络 I/O 返回。
- 观察线程只执行 shutdown，不 close，也不释放 TLS。下载线程等观察线程完成后，才关闭 TLS 会话和 socket、释放信号量，避免 fd 被重用或 transfer 被提前释放。
- 握手结束后同时设置 socket 与 TLS 非阻塞模式。写操作循环消费实际发送字节；读操作接受任意正长度短读。WANT_READ / WANT_WRITE 分别等待相应方向，检查 TLS 已缓冲数据，避免已解密数据仍等待 socket。0 / 其它错误作为失败。
- 每个错误、取消、重连与退出共用清理路径。创建第二个信号量或观察线程失败也释放已创建的资源。服务器要求关闭连接时，下一个 Range 请求建立新会话。

## LAN 所需算法

审计基于锁定 MiCO SDK 3.6.4、提交 `6c4652` 的 MOC 导出表（实际完整提交见 `common/mico-sdk.json`）。原工作区的 LAN 研究模块未加入常规构建；其依赖不能用普通固件的体积差推算。

| 算法 | 已见内核接口 | 处理方式 |
| --- | --- | --- |
| AES-128 单块 | `AesSetKeyDirect`、`AesEncryptDirect`、`AesDecryptDirect` | 可用于 LAN 的 CCM 单块运算，需按内核上下文 ABI 接入 |
| AES-CBC | `AesSetKey`、`AesCbcEncrypt`、`AesCbcDecrypt` | 直接导出；CBC 本身不能替代 CCM |
| SHA-256 / SHA-1 | MX1290 的扩展 `InitSha256` / `Sha256Update` / `Sha256Final` 等 | 扩展包装检查 `EXTRA_CRYPTO_FLAG`，缺失时返回 -1，不能假定所有已安装内核都支持 |
| HMAC | MX1290 扩展 `HmacSetKey` / `HmacUpdate` / `HmacFinal` | 同样需要检查扩展可用性；MW3xx 导出表没有同等扩展保证 |
| P-256 / ECDH / ECDSA | 未发现可直接调用的原语导出 | 允许保留应用侧最小实现及必要链接；`ssl_set_ecc` 只控制 TLS 功能，不是 ECC 原语接口 |

对应源码：`include/mico_security.h`，`platform/MCU/{MX1290,MW3xx}/moc/moc_api.h`，以及 MX1290 的 `moc_api_sep.h`、`moc_adapter.c`。当前 OTA 签名的 micro-ecc、SDK SHAUtils 不依赖可选扩展；写请求去重也复用同一 SDK SHA-256。将来启用 LAN 应单独验证 AES/哈希 ABI、CCM 向量、ECDH/ECDSA 向量和完整链接映射；当前变更没有删除原工作区未提交的 LAN 实现。

## 证据与限制

`python3 tests/test_ota_fetch.py` 使用生产下载代码及真实 OpenSSL TLS 的 MiCO API 主机适配，开启 ASan/UBSan。覆盖短读（23 字节）、短写（17 字节）、三种 WANT 方向、SNI、每段重连、畸形响应、握手停滞、总超时和取消；同一进程连续重试，注入信号量/线程/TLS 分配失败，每轮检查会话、信号量、线程及文件描述符归零。

A1 fresh-air 完整版在本地构建成功：应用 241476 字节（约 236 KiB），262144 字节应用区剩余 20668 字节（开发前端来源和 dirty 状态记录于该次 manifest）。最终发布构建以 CI 产物为准。

主机适配不执行专有 MiCO 内核，不能证明某一已安装内核的阻塞 API 在 shutdown 后的实际响应时间，也不能替代 A1/M1/TC1 的堆、栈和实机取消测试。本次授权实机是 ESP32 DAIKIN；MiCO 设备实测尚未进行。
