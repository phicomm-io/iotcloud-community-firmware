# TC1 JSON 浮点转换 ABI 修复（2026-10-19）

## 问题与复现

v0.9.14 发布后，TC1 `10.117.152.102` 的首页读取发生超时并重启；用户还报告 `.101` 有同样现象。停止浏览器后，对 `.102` 逐个执行只读命令：`capabilities.get`、`ota.sources.get`、`status.get` 成功，而单独执行 `power.get` 超时，随后设备 uptime 回到约 12 秒。重启前空闲内存约 37 KiB，JSON 动态分配失败计数为 0；因此不能把此现象直接归为 OOM。

`power.get` 将功率和电量作为 double 序列化。JSON 原有补丁给 newlib 浮点转换分配独立的 `struct _reent`，但 SDK 仅在链接时启用 `--specs=nano.specs`。编译实际引用标准 newlib 头文件，链接的则是 libc_nano，两者上下文布局不同。

使用固件同一套 ARM GCC 5.4.1 检查：标准布局大小为 1064 字节，nano 布局为 96 字节。原配置在 qemu-arm 中调用实际 nano 库浮点转换出现 SIGSEGV，编译时也启用 nano.specs 后成功。该实验运行真实 ARM libc，但没有模拟 MiCO 内核；实机未接串口，不能据此声称读到了 MCU fault/reset 寄存器。

## 改动

- JSON 库编译时统一使用 `--specs=nano.specs`，与既有链接配置一致。
- 私有上下文头文件要求 `_REENT_SMALL`；误用标准布局会在编译阶段失败。
- A1、M1、TC1 及主机测试的 SDK 有序补丁栈都增加第五个补丁，允许已有缓存从四补丁状态升级。
- ARM 回归测试提取 SDK 的实际 JSON double 序列化函数，验证错误配置被编译检查拒绝，正确配置下 100 轮、每轮四个数值的序列化和解析通过。TC1 CI 安装 qemu-user 并执行测试。

## 本地验证

候选提交：`7e67d1b894eeb8d526f78ca7b51fb4c8e09a436f`；运行版本 `0.9.14-7e67d1`。

- `python3 -m unittest tests.test_json_newlib_nano tests.test_apply_sdk_patch`：通过；6 个测试全部运行。补丁测试使用本地 TC1 SDK，验证完整五补丁栈从所有前缀升级及重复应用。
- TC1 完整固件构建：通过；产物 manifest 记录 clean 源码，实际 `json_object.d` 引用 `include/newlib-nano/newlib.h`。
- 构建并应用全部 SDK 补丁后，`bash tests/run_tc1.sh`：通过。构建前首次执行因 MQTT SDK 补丁尚未应用而在包读取断言失败；未改测试绕过此失败。
- `PHICOMM_TEST_JSON_DIR=build/tc1-sdk-3.6.4/mico-os/libraries/utilities/json_c SANITIZE=1 bash tests/run.sh`：通过。
- 签名包使用既有可信 P-256 密钥，并验证签名、源码提交、manifest 和原始 payload 一致。

## 实机候选验证

两台 TC1 各一次签名 OTA，HTTP 202，实际运行候选 `0.9.14-7e67d1`。不关闭 HA/MQTT，不改变插座输出。各执行 120 次顺序 `power.get`，再用真实 Chromium 打开完整首页约 45 秒，允许只读命令，禁止写命令及外部网络访问。

| 目标 | 120 次读取 | 读取时最低采样可用内存 | 网页成功功率读取 | 网页结束可用内存 | 网页期间 uptime |
| --- | --- | --- | --- | --- | --- |
| `.102` | PASS | 36264 B | 20 | 37624 B | 109 → 156 s |
| `.101` | PASS | 32984 B | 19 | 37352 B | 150 → 196 s |

两台网页均 ready，无页面错误或持续加载；最终 MQTT 在线，JSON 活跃分配为 0、分配失败为 0。最终复核 uptime 分别为 399 s、211 s，未观察到非计划重启；完整配置、插座状态、定时和按键均与此次升级前及 v0.9.14 部署前快照一致，六路输出保持开启。

浏览器结束时诊断累计最低采样可用内存分别为 29680 B、29496 B；该指标包含启动及观测期间的采样，不能等同所有分配时刻的真实峰值。这是原故障路径的有界回归，不构成永久无泄漏或任意负载绝不重启的保证。A1/M1 本轮实机验证未运行。

正式发布准备为 v0.9.15；上述实机使用修复提交候选，正式合并后的文档、测试与版本标识变化需与其应用差异分开核对。证据目录 `build/tc1-json-abi-device/` 为私有文件，含配置快照，不公开上传。
