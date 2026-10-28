# 分设备原厂安装教程与 OTA 工具调整

日期：2026-10-28。本文记录教程依据与本次本地检查，供维护和审阅使用。

## 公开教程组织

- `m1-no-disassembly`：先配网（SoftAP HTTP / Linux mac80211 EasyLink V2），再选择局域网上传或 DNS 主动下载。
- `a1-no-disassembly`：先配网（Linux mac80211 EasyLink V2；有条件优先官方 App），再选择局域网上传或基于 v1.47 分析的 DNS 下载。
- `m1-flash`：推荐串口 Bootloader，M1 模块焊点便于焊接，USB-TTL 模块常见；保留 J-Link。
- `a1-flash`：推荐通过工厂测试点免焊连接 J-Link；保留串口 Bootloader。
- `stock-provision`、`a1-m1-flash` 旧地址转到快速开始的设备选择表；`v1-features` 转到 Release Log 的 Beta 功能说明。两种语言均保留跳转。
- 下载导航与 Release Log 双向连接。版本历史按发布日期、版本、组件、变化类别和编号排列；发布日期另存为展示数据，不更改归档或已审核发布身份。

## 原厂行为与结论范围

M1 机身短按切换屏幕；约 3 秒事件由主控经 UART 通知联网模块进入配网，约 9 秒事件清除参数并重启。不是机身按键直接连接联网模块 GPIO。点位和触摸入口依据既有原厂主控 / 联网模块调查，见 `stock-provision-entries-20261028.md`。

用户在本次任务中确认 A1 原厂 OTA 接口可用，因此不再保留“只有兼容性讨论”的旧页面。此次编辑没有再次给设备刷写。免 App EasyLink 的成功条件按用户提供的最新实测范围统一为 Linux mac80211 帧注入，普通系统 UDP 发包不作为已验证安装途径。

A1 DNS 服务依据原厂 v1.47 镜像（SHA-256 `7fe89d1fa2c2c3a246fa6300f0e76c0ab81ad9fb06ae95ba7b0bc3d11d93d52a`）：

| 项目 | 原厂分析结果 |
| --- | --- |
| 激活 | `phicloudsym.phicomm.com:443/device/active?mac=…&productId=30` |
| 更新请求 | `aircleaner.phicomm.com:443/aircleanerfirmware/checkUpdate`，不附带 MAC 查询参数 |
| 更新字段 | 顶层 `url` / `md5` / `ver` / `enforce`，不使用 M1 的 `wifiSoftware` 包装 |
| 版本 | 去掉首字符后作浮点比较，v1.47 的示例返回 v1.48；仅为原厂比较标签 |
| 定时条件 | 每小时定时器；小时字符串为 `18`、已激活且 OTA 状态允许时检查，改 DNS / 重启不保证立即检查 |
| TLS | `ssl_connect(fd, 0, NULL, …)` 不验证证书，但仍需兼容旧 TLS 客户端 |
| 地址依据 | 更新处理 `0x0808873c`，字段解析 `0x08088a40`，下载 `0x08088c72 → 0x0808bd2c`，MD5 / 提交 `0x0808bc12 → 0x0808db20`，定时器 `0x0808a268` / 回调 `0x080894b0` |

DNS 服务的本地测试验证 HTTP/TLS 响应、型号和哈希拒绝、激活身份、更新清单及 Range；不等于 A1 实机 DNS 迁移成功。公网业务端口仅监听记录，不猜测或发送云端指令。

## 大金版本扫描约束

`--probe-versions FIRST LAST` 遍历同前缀、末段递增且包含端点的版本标签。每次间隔 2 秒，只发送握手和版本头；首个 `ToDo` 或通信异常立即停止。只有明确 `No` 后 EOF 才记录为拒绝。

不读取或发送固件文件，不发送 `End` / 校验提交，不切换 Wi-Fi，也不根据边界自动刷写。相邻拒绝 / 接受边界仅在同前缀、严格只接受更高版本的假设下推测当前标签；不是直接读取已安装版本或固件兼容性证明。上传必须另外指定镜像、SHA-256 和明确的 `--version`。

## 本地验证

- 大金协议测试覆盖分片响应、拒绝边界、接受即停、异常即停及真实 socket 对端抓取到的零固件正文。
- Linux C 工具以 `-Wall -Wextra -Werror` 编译；离线测试核对 UTF-8 长度、填充、PSK、Radiotap / FromDS、组播 MAC、包长和 IP 校验和；不调用无线网卡发包。
- A1 DNS 服务通过本机回环 HTTP/TLS 测试；`--check-only` 接受归档的 v0.9.27 A1 完整版原生 OTA，拒绝错误型号、哈希、CRC、MD5 和封装。
- 网站使用保留发布归档的正式校验构建，执行中英文导航、旧链接、OS 选择器、Release Log、手机布局和无 JavaScript 检查。具体结果保存在审阅目录。
