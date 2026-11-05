# TC1 重启与配置丢失：SDK 按键中断

日期：2026-11-04。适用固件 `1.0.0-rc.5`（基线提交 `8634a1c`）。

## 现象与触发

从网页或 MQTT 开关**带负载**的插座时，设备重启；多数情况下重启后 Wi-Fi、MQTT、插口名称和全部设置回到出厂值，需要重新配网。空载插座不触发。

同一台 TC1 A1（EMW3031，未配置 MQTT）实机测量。设备 ID 不记录在本页，镜像身份见文末：

| 操作 | HTTP 耗时 | 整包提交 | 结果 |
| --- | --- | --- | --- |
| `indicator.set` 值未变化 | 10 ms | 不写 | 正常 |
| `indicator.set` 改值 | 535 ms | 1 次 | 正常 |
| `outlet.set` 单路开/关 | 536–558 ms | 1 次 | 正常 |
| 全部关 → 逐路开（7 条命令）+ 并发 `status.get` | 0.53–0.56 s ×7 | 7 次 | 正常 |
| 风扇空载的同一插座反复开关 | — | — | 正常 |
| 风扇插在别的插座上开关 | — | — | **触发** |
| 风扇转动（37.2 W）时连发 20 条只改指示灯的命令 | 0.53–0.61 s ×20 | 20 次 | 正常 |
| 带负载插座通/断 | — | 中断 | 触发：重启；部分同时清空配置 |

由此排除：内存不足（`free_memory` 30.9–35.5 KB，`minimum_sampled_free` 25416，保留线 16384，`memory_denied`/`wire_alloc_failures` 均为 0）、Flash 提交过慢（0.55 s）、计量脉冲与提交冲突（20 次提交无异常）、某一插座或 `GPIO18` 的代码路径（同插座空载正常）。触发条件是**触点带电流通断**。

一次带负载复位后核对：`config_source_version=12`、`config_auth_valid=true`、Wi-Fi 与名称仍在，插口状态回滚到上一次成功提交。**被中断的提交是安全的**，不会写坏；因此“数据全丢”只能来自显式擦除。

## 根因

`board/MK3031/platform.h` 的 `EasyLink_BUTTON` 就是 `MICO_GPIO_23`，与 TC1 电源键同一引脚。SDK 在该引脚注册下降沿中断（`libraries/drivers/keypad/gpio_button/button.c`），并在 `RestoreDefault_TimeOut = 3000` ms 后调用 `PlatformEasyLinkButtonLongPressedCallback()`：

1. 擦除 `PARAMETER_1` 与 `PARAMETER_2`（Wi-Fi 凭据、MQTT、插口名称、定时任务都在内）；
2. `mico_system_power_perform(eState_Software_Reset)` 重启。

该中断处理函数在**中断上下文**里调用 `mico_rtos_get_time()` 与 `mico_rtos_start_timer()`，且定时器到期时不复查引脚电平。触点拉弧在按钮脚上产生的一个边沿，就等于 3 秒后擦盘重启。

[a2633063/zTC1](https://github.com/a2633063/zTC1) 没有这个问题，差异在于它的按键初始化把该引脚的中断抢过来自己用，SDK 的处理函数因此不再被调用：

```c
mico_rtos_init_timer(&user_key_timer, 100, key_timeout_handler, NULL);
MicoGpioEnableIRQ(Button, IRQ_TRIGGER_FALLING_EDGE, key_falling_irq_handler, NULL);
```

本移植把按键改成 40 ms 轮询（`KeyInit` 只启动定时器，不再调用 `MicoGpioEnableIRQ`），SDK 的处理函数因此一直存活。两版都定义了 `MICO_SYSTEM_MONITOR_ENABLE`，差异不在监控。

## 修改

`TC1/easylink-button-guard.patch`（仅 TC1 应用）：

- `button.c` 的下降沿中断只调用产品计数钩子后返回：不重挂边沿、不读 RTOS 时钟、不启动定时器，中断上下文不再进入 SDK 按键状态机。未被引用的上升沿处理函数一并删除。
- `PlatformEasyLinkButtonLongPressedCallback()`：不再擦分区、不再重启。
- `PlatformEasyLinkButtonClickedCallback()`：只计数后返回，不再启动 SDK 自己的 AutoConfig 配网；它运行在引脚中断上下文，除弱符号计数外不做任何 I/O 或 RTOS 调用。
- 新增弱符号 `app_easylink_button_event(unsigned long_press)`，TC1 在 `main.c` 中只做计数，无分配、无锁。

`TC1/system-monitor-trace.patch` 只增加一行日志：监控点超时重启前记录点位、实际停顿和上限。

产品自身的按键路径不变：`KeyInit` 仍以 40 ms 采样 `GPIO23`，5 秒配网 / 10 秒恢复出厂手势照常，恢复出厂仍通过 `tc1_auth_reset(true)` 显式清除配置。

## 接口与配置

- 协议、命令、错误码、请求 ID 与写重试语义均未改动；配置布局未变（`USER_CONFIG_VERSION` 仍为 12），无迁移与降级要求。
- `GET /api/v1/memory` 新增四个字段（由同一分支的 `feat(tc1)` 提交先引入，本提交只带 SDK 侧修复）；读取方按约定容忍未知字段，旧客户端不受影响：
  `last_reset_reason_text`、`boot_defaults_applied`、`boot_version_mismatch`、`button_edges_without_gesture`。
- 新增字段只含非敏感的诊断值，不含凭据；串口/服务日志沿用既有英文标识符，无新增用户可见文案，因此不需要 i18n key。

## 验证

| 项 | 结果 | 证据 |
| --- | --- | --- |
| 主机回归：`tests/test_tc1_button_guard.py`（guard 补丁不含擦除/重启/中断重挂调用）、`tests/test_tc1_reset_fields.py`（启动标志映射、字段暴露、标志置位顺序） | PASS | `tests/run_tc1.sh` |
| 关联既有回归：`tests.test_tc1_status_dispatch`、`tests.test_service_mp`、`test_tc1_core.py`、`test_tc1_gpio.py`、`test_tc1_wifi.py` | PASS | 本地运行 |
| SDK 补丁栈：整套 `TC1/build.sh` 补丁在 MiCO `6c46521` 上连续应用两次 | PASS | 可重复应用 |
| 固件构建：TC1 `full` 变体（内嵌 WebUI，设备首页可打开） | PASS | 见文末产物身份 |
| 固件构建：TC1 `no-frontend-resources` 变体（无 WebUI） | NOT_RUN | 本机无法获取 pinned 前端 release 资产（下载被中断）；由 CI 覆盖 |
| A1 / M1 / DAIKIN / S30 构建与回归 | NOT_RUN | 本改动只涉及 `TC1/` 与 TC1 专用 SDK 补丁，未改 `common/` |
| 实机：刷入候选镜像（`full` 变体，内嵌 WebUI）后经 MQTT 与网页多次开关带风扇插座 | PASS | 同一台 TC1 A1；连续运行 1.2 h 无重启；`boot_defaults_applied=false`、`config_recovery=false`、配置与 Wi-Fi 保留；`button_edges_without_gesture=16` 说明干扰边沿确实到达该引脚且不再产生后果 |
| 实机：长按 5 秒配网、10 秒恢复出厂 | NOT_RUN | 未在本轮验证 |
| 实机：候选镜像连续运行 25.2 h（1.05 天） | PASS | 同一台 TC1 A1 与镜像 `1.0.0-rc.5-8634a1-dirty`；期间经 MQTT 与网页开关带负载插座，`uptime=90816 s` 无重启；`boot_defaults_applied=false`、`config_recovery=false`、配置与 Wi-Fi 保留；`button_edges_without_gesture=18`（13 h 时为 16，干扰边沿仍在到达该引脚，无后果）；`free_memory=23968`、`minimum_sampled_free=22160` 与 13 h 时完全相同（无增长）；`memory_denied`/`wire_alloc_failures`/`mqtt_overflow` 均为 0 |
| 实机：25 h 服务日志 | PASS | 全量 63 条：61 × `command/accepted`、`system/boot`、`system/software`（刷机那次），无任何 warn/error，无 `storage_error`/`rejected`/`missed` |

未复现“被中断的提交同时毁掉两份副本”：带负载复位后配置回滚到最后一次成功提交，Wi-Fi 与设置保留。

## 观察点

- 启动日志：`reset reason raw[0x..] <原因>`（MC200/MW3xx 启动标志：`0x08` 软件、`0x20` 看门狗、`0x10` lockup，低三位为电源域事件），并向服务日志写入一条对应事件。
- 启动日志：参数分区不可用时记录 `WARNGIN: parameter partitions unusable at boot, defaults applied`。
- `button_edges_without_gesture` 统计该引脚上的边沿（真实按键与干扰都计入）。修改后它只增长，不再触碰 Flash。

拉弧本身仍可能造成 MCU 复位（电气层面）；若更换更重的感性负载后出现“只重启、配置保住”，那属于电源/EMI 余量问题，需要触点吸收等硬件措施，与本修复无关。

## 候选产物

由本分支的独立检出构建（基线 `main` = `a41f4d5`，工作区干净；镜像构建于 `c8abb5e`，此后只有文档提交），`full` 变体：

| 项 | 值 |
| --- | --- |
| 变体 | `full`（内嵌 WebUI；设备 `/api/v1/status` 报 `firmware_variant=full`、`full_resources=true`） |
| 版本 | `1.0.0-rc.5-c8abb5`（短哈希是构建时的 HEAD；本分支之后只有文档改动，重编只会改变这一串） |
| `phicomm_tc1_a1.ota.bin` | 632668 B，SHA-256 `0c60a77eb7dddf844f69c2efdc81f5edb5cc566d6a6a8e667181bc2c784bd7fc` |
| `.elf` text / data / bss | 221788 / 1252 / 34096 B |
| 工具链 | ARM GCC 5.4.1（本地为 2016q3 构建，见下） |
| SDK | MiCO 3.6.4 `6c46521` |
| 前端 | `bf58948`（本地源码，`manifest.json` 标 `development`） |

该产物是**未签名**镜像，只能经 `/api/v1/ota/upload-unsigned` 或网页“允许未签名固件”刷入；发布签名只在 `main` 的标签上生成。

## 本页面之外的已知偏差（仅本地构建环境）

- 本地工具链是 ARM GCC 5.4.1 的 2016q3 构建（`-dumpversion` 同为 5.4.1），pinned 的 2016q2 tarball 在本网络下无法获取；
- 32 位工具链经 Ubuntu `libc6-i386` 的 `/tmp/l32` loader 运行（无 multilib、无 root）；
- MiCO 的 `makefiles/micoder_host_cmd.mk` 在无 `dash` 的主机上需改用 `$(file ...)` 写构建文件（主机侧，不影响生成代码）；
- 前端由固定 commit 的本地源码渲染，而非 pinned release 资产（该资产下载被中断）。

以上均不影响 `TC1/` 与两个 SDK 补丁的内容；CI 使用 pinned 工具链与 release 资产重新构建。
