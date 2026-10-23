# A1 新风扩展

基于 A1 主线，新增 `fresh-air` 和 `fresh-air-no-frontend-resources` 两个固件变体。设备型号仍为 A1；普通版、普通精简版、新风版、新风精简版的 OTA 路径严格分开。普通完整旧版本允许的无 variant 元数据兼容路径不适用于新风版。

## 接线与 PWM

- 所有 A1 原风机 PWM 均改为 25 kHz。原驱动板映射不变：GPIO12 / PWM3，GPIO19 低有效使能，运行占空比 `80 - 0.6 × speed`。
- 新风版用户串口 TX / GPIO9 复用为 PWM1，RX / GPIO10 为上拉、下降沿反馈输入；不再用于用户串口。
- 新风反馈为脉冲累计数和 Hz（协议单位 mHz），没有假设每转脉冲数，不报告 RPM，也不作为失速保护或闭环控制依据。灯环发送期间的中断屏蔽可能影响计数，须实测。
- 新风实际高电平占空比 `D = clamp(a × S + b, pwm_min, pwm_max)`；S 是最终输出的归一化转速百分比。a、b 支持有限小数，a 可为负；`0 ≤ pwm_min ≤ pwm_max ≤ 100`。
- 高电平开启预设 a=1、b=0；低电平开启预设 a=-1、b=100。极性仅决定关机电平，不重复反转公式。S=0、关机绕过映射；0/100% 用常量 GPIO，中间值用 25 kHz PWM。

## 联动和自动模式

新风开关、手动速度、自动策略独立保存。出厂关闭，记忆手动速度 60%。自动默认使用已绑定 M1 的 CO₂，800–2000 ppm 线性映射到 20–100%，传感器离线回退 30%，回退也受自动范围约束。支持 PM2.5、甲醛、CO₂ 组合，固定、线性、PID、统计方式和启停回差，使用独立控制器状态。查询状态不会推进 PID。

两种模式分别保存比例：

1. `coupling=0`：新风输出 `F ≤ floor(P × cap_ratio / 100)`，比例 0–100，默认 100。
2. `coupling=1`：原风机输出 `P ≥ ceil(F × boost_ratio / 100)`，比例 1–500，默认 100。原风机自动和夜间上限优先；不足时降低新风。

上述 P、F 是输出命令百分比，不是反馈测得的机械转速。每个 100 ms 调速步骤均应用约束；上升先提高原风机，下降先降低新风。正常升速每步最多 10%，约束收紧时允许立即降速。原风机输出失败会撤销新风输出。

显式关闭原风机/整机立即停止两者，但保留新风请求。原风机自动待机不等于整机关机，抬高模式仍可请求原风机转动。

## 启动与重启电平边界

应用平台初始化先关闭原驱动板使能。新风 TX 在未知持久化极性时采用无上下拉高阻；配置读取后、网络/HTTP/硬件线程启动前预置输出锁存值，再切换输出模式为所选关机电平。后续恢复保存的运行请求。切换映射会先停止新风，再应用新参数。

正常重启及 OTA 最终重启通过输出互斥锁关闭两路并锁止重新启动。应用无法保证冷上电 ROM、引导程序或瞬时看门狗复位期间的电平；需示波器实测，必要时增加匹配极性的外部上下拉/使能门控。本改动不修改引导程序。

## 服务接口

新风变体的 `capabilities.get` 返回 `fresh_air: true` 并声明以下 action：

- `fresh_air.set`：可选 `on`（布尔）、`speed`（0–100 整数）、`mode`（`manual`/`auto`）。切换模式不会开机，0 速度停转。
- `fresh_air.config.get` / `.set`：`revision`、`coupling`、`cap_ratio`、`boost_ratio`、`active_low`（布尔）、`a`、`b`、`pwm_min`、`pwm_max`。set 必须携带当前 revision，部分更新原子校验；过期返回 conflict。
- `fresh_air.linkage.get` / `.set`：与原 `linkage` 相同的策略字段，独立 revision（与新风其他设置共享版本号）。该策略的 enabled 固定为 1，是否使用由新风 mode 决定。

配置追加到既有存储末尾，带 magic/schema。原有存储前缀和版本 9 ABI 不变，新普通 A1 也保留该扩展。控制和参数保存沿用 A1 延迟合并写入，`config_dirty` / `save_error` 表示持久化状态。

live/full 状态新增 `fresh_air_requested_on`、`fresh_air_requested_speed`、`fresh_air_mode`、`fresh_air_on`（输出命令非零）、`fresh_air_target`、`fresh_air_speed`、`fresh_air_primary_speed`、`fresh_air_pwm_milli`、`fresh_air_fb_millihz`、`fresh_air_pulses`、`fresh_air_limit_reason`、`fresh_air_ready`、`fresh_air_fallback`。限制原因：0 无、1 整机关机、2 调速/比例约束、3 输出错误、4 应用配置。

前端复用同一 A1 页面，按 capability 显示新风卡片和 `#settings/freshair`、`#settings/freshauto`。MQTT 使用既有统一 command topic，HA 增加独立新风 fan（含模式）和反馈频率 sensor；即使原风机加入分组，新风仍属于物理 A1。切回新普通固件时发布发现实体的 retained 空消息。

## 构建与验证

```sh
PHICOMM_FIRMWARE_VARIANT=fresh-air ./A1/build.sh
PHICOMM_FIRMWARE_VARIANT=fresh-air-no-frontend-resources ./A1/build.sh
```

本地未发布的前端锁定包需通过 `PHICOMM_FRONTEND_ARCHIVE` 指定。CI/正式发布前必须先发布相应前端 commit 的锁定 release。此开发不执行设备刷写、OTA 或公开发布。

自动化测试覆盖比例全域、映射、调速不变量、控制命令与 revision、传感器回退、重启锁止、反馈计数、浏览器配置及 OTA 路由。硬件验收仍需实测双路 25 kHz、驱动板转速映射、正反极性、冷启动/重启/OTA 电平、FB 丢脉冲及断电保存恢复。
