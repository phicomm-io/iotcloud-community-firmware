# TC1 / M1 / A1 MiCO SDK 统一与差异记录

日期：2026-10-16。最初验证基线为 `d4911ba79e6368eb8e3f81378da9b6525780ff2b`；合并候选已重新集成到 `f94b1c1`（v0.9.6）主线。下方保留首次构建、单台 TC1 部署与内存对比的历史记录，最新主线回归见文末。

## 统一后的构建来源

三个型号都读取 [`common/mico-sdk.json`](../../common/mico-sdk.json)，使用 MiCO **3.6.4**、提交 **`6c465211d3ff8797cd835e400ec54a06530dd476`**。原 TC1 使用的 3.5.4 发布归档不再参与当前构建。

[`tools/prepare_sdk.py`](../../tools/prepare_sdk.py) 下载一次公共 Git 源，默认缓存到 `build/mico-upstream.git`，校验提交后为各产品创建 checkout。A1/M1 的板级 GPIO 补丁不同，保留独立的工作目录；统一的是源码来源、版本和准备流程。TC1 改用 `build/tc1-sdk-3.6.4/mico-os`，避免覆盖旧归档缓存。构建、CI 和三个产物清单都使用公共版本信息。

公共缓存支持 `MICO_SDK_CACHE`，离线来源支持 `MICO_SDK_REPOSITORY=/path/mico-os-3.6.4.bundle`。已有 checkout 也会检查提交；错误版本、非 Git 目录和不完整目录不会被静默覆盖。

## 可以看到哪些差异

对本机旧 TC1 SDK 撤销仓库原有的八个 overlay 后，与上游 3.6.4 的 Git HEAD 比较；文本比较统一 CRLF/LF。旧侧是历史 TC1 定制归档的还原快照，**不是声称未经修改的官方 3.5.4 tag**。

结果：新增 **2,199** 个文件、删除 **4** 个文件、修改 **146** 个文件；修改项中 **114** 个是文本，**32** 个是二进制。新增项包含许多其他芯片、板卡和平台，不能全部算作 TC1 的功能变化。

本地完整证据（位于忽略的构建目录，不随源码提交）：

- [文本完整 diff](../../build/evidence/mico-sdk-3.5-to-3.6/source.diff)
- [文件清单、旧包来源和所有变化二进制的大小 / SHA-256](../../build/evidence/mico-sdk-3.5-to-3.6/inventory.json)

| 范围 | 看到的变化 | 本次处理与影响 |
| --- | --- | --- |
| 参数保存 | 3.5 的 CRC 紧跟应用配置；3.6 改为参数分区末尾的 CRC，并调整锁和旧格式回退 | 重做 TC1 参数补丁，先校验旧 CRC / magic，再支持当前配置及冻结的 v11 / v10 布局迁移；读写失败返回错误。主机 Flash 模拟通过；指定 TC1 的配置读回保留已验证，其他设备未验证 |
| HTTP 认证 | 旧 TC1 归档含 Basic Authorization 解析和分发前认证；上游 3.6.4 缺少这部分定制 | 提取为显式 `TC1/http-auth-sdk.patch`，保留 GET / HEAD / POST / PUT / DELETE 认证，以及已经解析头部时的处理规则 |
| HTTP 二进制上传 | 旧归档含 `httpd_get_data2`，上游没有 | 在 HTTP 补丁中显式提供二进制读取器；不追加 NUL，限制在剩余正文内读取，处理断流及错误；继续保留 HTTP 超时和关闭连接修复 |
| MQTT 连接失败清理 | 旧归档在六个失败分支关闭 socket 后置 `-1`，上游缺少这些赋值 | 提取为三款共用的 `common/mqtt-socket-close.patch`，避免后续清理重复关闭已回收的描述符 |
| MQTT 包与响应超时 | TC1 旧补丁适配 3.5；M1/A1 已用 3.6 补丁 | TC1 改用同一套 3.6 MQTT、包期限、响应期限及 PUBLISH 长度检查补丁，删除旧版本 overlay |
| Wi-Fi 配网 | EasyLink 接口改变，增加 AWS 和可选多 AP 支持 | TC1 继续用自己的 Wi-Fi 管理，复用 3.6 的 SDK Wi-Fi 所有权补丁；不将上游新增配网能力当作 TC1 已启用功能 |
| 系统监控 | 超过已注册监控点期限时新增显式 `MicoSystemReboot()` | TC1 的 SDK 默认监控期限仍是 5 秒；此次升级不等同于完成业务线程卡死监控 |
| RTOS / 内核接口 | 增加 `mico_rtos_thread_yield` 等接口；MK3031 内核二进制改变 | 使用配套的 3.6.4 头文件、接口和内核；闭源内部修改无法由源码 diff 确认 |
| Flash / 链接 | MK3031 分区地址保持一致；diff 中扩大的 `app_with_moc_b.ld` 对应 MK3031B | TC1 使用 MK3031，不能据此声称 TC1 应用空间扩容 |

## TC1 内核和升级边界

`resources/moc_kernel/3031/kernel.bin` 从 **363,228** 字节变为 **363,508** 字节：

| 文件 | 旧 SHA-256 | 新 SHA-256 |
| --- | --- | --- |
| MK3031 kernel.bin | `d56d3f11bc2b4b4123257c9d27df1132b9f18cfc3973f1bee7ba2833df4f1161` | `2f5dd55f8e702ff42c21d2606ec553731ce1cdefc2a90694d430559351e12874` |
| MW3xx boot2.bin（均为 24,576 字节） | `1628c000291090e4792498d4fbe4abbfa329eecdd069781424cc8a7689b5c579` | `2ec6ae3b4854807f7ba4981fa24b1395edf2ed96f98179412b275edfa918a9f3` |

新旧二进制不同是可确认的；不能凭大小或哈希断言内部修复了某个 Wi-Fi / TCP 问题。

TC1 签名 OTA 适配器检查内核及其填充区的固定 MD5，以限制应用升级范围。该值从 `df2641dcca61b30be154b50bcdf25003` 更新为 `0677cf68fca7dfdc9b81228d2a1b7ec3`。**运行旧固件的设备会拒绝直接通过原签名 OTA 路径安装这个新内核，即使外层签名正确。** 本次没有设计自动过渡升级。用户随后明确授权使用应用提供的未签名恢复通道完成单台迁移，详见部署记录；SDK 自带的未签名写入入口继续禁用。

## 验证结果

| 项目 | 结果 / 证据 |
| --- | --- |
| TC1、M1、A1 实际 ARM 固件构建 | PASS：[最终构建日志](../../build/mico36-all-builds-final.log)，三个 manifest 的 SDK commit 完全相同 |
| 公共准备器创建全新 TC1 checkout、应用补丁并构建 | PASS；另有[最终重复构建日志](../../build/mico36-tc1-idempotent-final.log)，验证补丁可重复应用 |
| `tests/run.sh` | PASS：[公共测试日志](../../build/mico36-common-tests.log)，包括公共 SDK 准备器的离线缓存、版本拒绝、目录保护测试 |
| `tests/run_tc1.sh` | PASS：[最终 TC1 测试日志](../../build/mico36-tc1-tests-final.log)，含实际 SDK 参数迁移 / Flash 故障、HTTP 认证、二进制上传、socket 失败后重复清理测试 |
| `tests/build_host.sh` | PASS：[主机 worker 构建及协议测试日志](../../build/mico36-host-tests.log)，覆盖三款 SDK 的 MQTT 包处理、边界与 SUBACK |
| `tests/ota/run.sh` | PASS：[最终 OTA 测试日志](../../build/mico36-ota-tests-final.log)，包括实际生成 TC1 镜像的适配器验证、签名和 Flash 故障测试 |
| 隔离 Mosquitto 上三款实际 SDK worker | PASS：[Broker 日志](../../build/mico36-broker-tests.log)，覆盖发现、命令、模板、保留消息、去重、HA birth，以及 TC1 Legacy；硬件为主机桩 |
| 指定 TC1 的新内核迁移、启动、联网和配置读回 | PASS；见下方部署记录 |
| 物理控制、掉电恢复、长期稳定性及其他设备迁移 | NOT_RUN |

本机磁盘空间不足以再存一份 SDK，TC1 构建通过 `TC1_SDK_DIR=/dev/shm/phicomm-tc1-mico36-clean` 使用公共准备器生成的 checkout；默认路径已在 CI 和相关测试脚本中同步。三款构建版本均为 `0.9.5-d4911b-dirty`，不是已发布版本。

| 首次验证 OTA 产物 | 字节数 | SHA-256 |
| --- | --- | --- |
| TC1 | 618,940 | `fca76827d7afcbcae5463f919cfca383e62f252729c15a16b1ca45e7dd9541f8` |
| M1 | 634,360 | `3cf8c2cbadb076410520b2c80b7fb1cfa98f2e4265a3fca9c3d92a590d06214c` |
| A1 | 659,696 | `b65b39b678a7ad2cf9120b5fb8438855bb98649ee2828052c11150a7898bfcc1` |

对应 `build/{tc1,m1,a1}-release/manifest.json` 和 `SHA256SUMS`。以上为本地未发布构建的原始 OTA 文件；后续重新构建需以新的清单为准。

## 用户指定 TC1 的部署

用户明确授权向 `10.117.152.102` 使用不验证签名的通道刷入。现场确认设备 ID 为 `D0BAE463E12C`，旧版本为 `0.9.6-f94b1c`（仍使用旧 SDK），配置结构与本次应用基线相同。为测试 MiCO 3.6.4，本次应用代码暂时回到 `0.9.5-d4911b-dirty`，不包含 `f94b1c` 后续 OTA 功能修改。

保存配置、定时及按键映射快照并验证上述 TC1 镜像哈希后，仅向 `/api/v1/ota/upload-unsigned` 上传一次，收到 HTTP 202。设备重启后身份不变，版本为 `0.9.5-d4911b-dirty`，Wi-Fi / MQTT 恢复连接，OTA 状态回到 idle。配置查询、定时映射、34 条按键映射和用户控制状态的前后比较均一致；六路插口状态均保持开启。没有通过命令切换插口或修改配置。

[部署读回结果](../../build/evidence/tc1-mico36-deploy-20261016/summary.json)记录精确镜像哈希及时间。配置快照存于该证据目录的权限 0700 的 `private/`，文件权限 0600，不随源码提交。此次结果仅证明这台设备完成部署和运行读回；物理输出及功能体验由用户继续测试。

### 内存读数对比

使用相同的 `status.get` 命令读取 `MicoGetMemoryInfo()->free_memory`，表示空闲堆字节数，数值越大可用空间越多。

| 固件 | 采样范围 | 空闲堆 |
| --- | --- | --- |
| 刷机前 `0.9.6-f94b1c` | 运行 2,180 秒时的单次快照 | 40,848 字节 |
| 刷机后 `0.9.5-d4911b-dirty` | 启动后 10 秒的一次读数 | 36,288 字节 |
| 刷机后连续采样 | 启动后 25–71 秒，20 次，间隔至少 2 秒 | 最小 37,920，最大 39,168，平均 38,877.2，中位数 39,028 字节 |

新固件采样中位数的空闲堆比刷机前单次值少 **1,820 字节（约 1.78 KiB）**。全部 20 次采样中 Wi-Fi / MQTT 均已连接。期间未发控制或配置修改命令。

这是两份实际固件的运行读数对比：应用基线、启动时长和采样数量也不同，不能归因为 MiCO 单独增加了 1.78 KiB 占用；也不表示总 RAM、峰值、最大连续空闲块或长期内存泄漏测试结果。[完整采样](../../build/evidence/tc1-mico36-deploy-20261016/memory-comparison.json)已保存。


## 最新 main 集成回归

合并候选基于主线 `f94b1c1`，保留 v0.9.6 的 TLS 下载、OTA 取消、active/passive 分区访问及 HTTP 客户端资源限制。新版 MiCO 的 HTTP 栈初始值为 8 KiB；`TC1/http-resource-sdk.patch` 已重新适配，候选继续使用主线规定的 12 KiB HTTP 栈。

重新执行并通过：

- TC1 / A1 / M1 的实际 ARM 构建；三个清单均指向同一个 MiCO 提交。
- `tests/run.sh`、`tests/run_tc1.sh`、`tests/build_host.sh` 和隔离 Broker 的 `tests/run_mqtt_v2.sh`。
- `tests/ota/run.sh`，三款均使用实际新建镜像，包含签名校验、Flash 故障和升级租约释放；没有跳过 A1/M1 镜像测试。
- `tests/test_ota_fetch.py` 的真实 TLS 与 sanitizer 用例，以及签名包验证、OTA Worker 测试。
- 中英文依赖指南与翻译源哈希检查。

本轮日志保存在隔离集成工作目录的忽略 `build/` 目录。后续 PR 检查、目标设备验证和最终 main 部署以对应提交、镜像清单及独立设备证据为准；本文历史镜像哈希不用于声明最终 main 产物身份。
