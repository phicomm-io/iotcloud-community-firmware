# M1 / A1 绑定、局域网和联动

## 配置入口

A1 与 M1 的离线 WebUI 新增「绑定与联动」。先设置同一个 MQTT Broker，在任一端填写对方 MAC，等待 `/binding/status` 中的绑定和数据状态。绑定令牌经非 retained MQTT 消息协商后保存在参数分区；后续局域网通信不依赖 Broker 在线。原生配置 v1 升级为 v2 时保留 Wi-Fi、MQTT、网页密码和手动控制设置。

每台 A1 最多关联 8 个 M1；M1 最多保存 8 个 A1 目标。独立 A1 可共享同一 M1。净化器组最多 8 台 A1，需要在每台 A1 上填写相同组名称与成员 MAC 列表；M1 可只填写组名称，通过 MQTT 地址公告建立该组成员的绑定。组配置统一，组主控制的 60% 会让每台 A1 都运行在 60%。高级设备控制主题可单独控制某台 A1。

M1 的联动、灯效、定时、睡眠和时钟页面配置绑定列表中的第一个 A1；该目标属于组时，使用组中在线 MAC 最小的 A1。提交返回 202 表示异步提交，最终结果见 `/binding/status` 的 `remote_result`，或者 MQTT `result` 消息。列表重排可选择其他目标。

| HTTP GET / POST | 用途 |
| --- | --- |
| `/binding/config` | 组、成员、绑定、LAN 地址和传输策略 |
| `/binding/status`（只读） | 数据可用性、传输、请求与组成员超时 |
| `/binding/probe`（POST `{"mac":"..."}`） | 手动探测已绑定的 M1 / A1 或组成员 |
| `/linkage/config` | 启停阈值、聚合、固定/线性/PID 风量 |
| `/light/advanced` | 默认灯效、分段显示和数据范围 |
| `/schedule/config` | 最多 8 个每周定时开关事件 |
| `/sleep/config` | 最多 4 个可跨午夜的睡眠窗口 |
| `/time/config` | A1 NTP 主机和 UTC 时差（分钟） |

保留 `/fan/config`、`/light/config`、M1 亮度和原有网络/OTA API。配置查询返回 `revision`，修改可以携带该值拒绝过期修改。MQTT 修改可携带 `request_id`，响应会回显。

## MQTT 主题

MAC 在逻辑主题中采用小写冒号格式，原生主题仍采用 12 位紧凑 MAC。

| 关联 | 逻辑根 B |
| --- | --- |
| 独立 A1 关联一个 M1 | `a1mac:m1mac` |
| 独立 A1 关联多个 M1 | `a1mac:00:00:00:00:00:00` |
| 命名组关联一个 M1 | `组名称:m1mac` |
| 命名组关联多个 M1 | `组名称:00:00:00:00:00:00` |

例如 `aa:bb:cc:dd:ee:01:11:22:33:44:55:66`，没有 `phicomm/` 前缀。

- `B/{binding,linkage,schedule,sleep,time}/{get,set,state,result}` 和 `B/light/config/{get,set,state,result}`：查询、修改、保留配置与非保留结果。set/get 不得 retained。灯效配置使用 `light/config`，与 HA 的 `light/set` 控制和 `light/state` 状态分开。
- `B/fan/{set,state}` 和 `B/fan/percentage/{set,state}`：ON/OFF、0–100%。
- `B/light/{set,state}`：HA JSON 灯控制；`B/child_lock/{set,state}`、`B/led_always_on/{set,state}`：ON/OFF。
- 单 M1：`B/sensor`；多 M1：`B/<m1mac>/sensor`；测量键为 `temperature`、`humidity`、`pm25`、`formaldehyde`，单位分别为 °C、%、µg/m³、mg/m³。
- 可选 CO₂：样本和平均值包含 `co2_supported`、`co2_valid`、`co2`（ppm），平均值另含 `co2_sample_count`。无效 CO₂ 为 JSON null，20 秒过期，独立于原四项读数的有效期。联动 `metrics` 位掩码为 1=PM2.5、2=甲醛、4=CO₂，可组合为 1–7；CO₂ 阈值为 `on_co2`、`target_co2`、`low_co2`、`high_co2`。
- `B/average`：新鲜且有效 M1 的算术平均值，与联动选用的统计策略无关。
- 组设备高级控制：`B/<a1mac>/fan/...`、`light/...`、`child_lock/...`、`led_always_on/...`。

绑定完成后删除原生 HA retained discovery，保留原生控制与信息主题；解除绑定恢复原生发现。新的 HA 实体共享逻辑设备标识，测量不 retained，`expire_after` 为 15 秒。可在 HA 重新上线后恢复发现。原生 MQTT LWT 用于逻辑实体的可用性，组采用任一成员在线策略。

首次 MQTT 绑定可向 `phicomm/a1/<紧凑MAC>/binding/set` 或 `phicomm/m1/<紧凑MAC>/binding/set` 发送：

```json
{"request_id":"bind-1","peers":[{"mac":"11:22:33:44:55:66","ip":"192.168.1.10","recovery":true,"timeout_ms":1000,"recovery_seconds":60}]}
```

清空 `peers` 解除关联。组名称不超过 32 字节，禁止 `: / + #` 和控制字符。

## 传输与故障

专用 UDP 单播端口 **11288**，单包上限 1200 字节。报文携带协议版本、源/目标 MAC、启动标识、请求 ID、配置版本、操作与绑定令牌。优先采用手工 IPv4，否则读取 retained MQTT 地址公告；探测回复必须匹配绑定 MAC、令牌和请求 ID。两次探测成功后采用 LAN，默认 1 秒超时并回退 MQTT，默认 60 秒检查恢复，可关闭恢复检查或强制 MQTT。超过单包上限的配置事务直接回退 MQTT。

各设备有独立请求、超时和恢复状态；某一接口超时不会阻塞其他设备。新启动标识、样本序号和采样年龄参与缓存校验，重复数据不续期。所有 M1 不可用时保持当前风机状态、报告 `sensor_failed`，定时、睡眠、手动控制和其他接口继续工作。配置和控制发送队列最多 4 项、总有效负载最多 4096 字节；拥塞时返回失败或在后续轮询重试。

组使用 5 秒心跳、15 秒失联判定，在线 MAC 最小者发布逻辑状态。网络分区可能暂时产生多个协调者；恢复通信后重新选择协调者并同步配置，不提供分区下的强一致性。

## 联动、灯带与时间

启停条件分别选择 PM2.5、甲醛或二者 AND/OR；启动优先于同时满足的关闭条件。默认持续 15 秒、最短开关间隔 60 秒。固定风量默认 60%，线性/PID 范围默认 20–100%。PID 以 `max(所选浓度/目标)-1` 为误差，默认 Kp=20、Ki=0.02、Kd=0，周期 5 秒，带积分抗饱和和导数滤波。参数需结合真实设备和房间调试。

统计策略为平均、最大、最小、中位数（有效数 >3，否则平均）、去最高最低平均（至少 3 个，否则平均）、按绑定顺序故障切换并自动回到恢复的优先设备。

睡眠的风机上限默认 30%，作用于所有控制来源；「禁止自动启动」只禁止联动从关闭转为开启，允许手动或定时开启。定时 OFF 的禁止自动启动状态持久保存，定时 ON 或手动 ON 清除；时钟无效时暂停时间事件与睡眠判断。

灯带默认保留原风速条，也可选择固定颜色、呼吸、彩虹、数据显示。逻辑 32 颗灯，1/2/3/4 段分别为 32、16+16、11+11+10、8+8+8+8；多段时各段最后一颗熄灭作为分隔。预设分别为空气质量，PM2.5+甲醛，温度+湿度+空气质量，温度+湿度+PM2.5+甲醛。每段可自定义颜色、数值范围和适宜区间，颜色与亮度分别表达适宜性/数值。数据模式不叠加 Wi-Fi 指示；睡眠亮度上限覆盖全部灯效，包括 Wi-Fi 指示。
