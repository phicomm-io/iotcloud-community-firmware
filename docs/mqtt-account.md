# MQTT 协议与账号隔离

MQTT 协议只有两种：**TMCP（Tom MQTT Cloud Protocol，v2）** 和 **Legacy（v1）**。`community` 和 `account` 不再是协议版本名称。

M1/A1 固定使用 TMCP，旧配置迁移后也使用 TMCP。TC1 新配置默认 TMCP，已保存的 Legacy 配置保留 Legacy。TC1 可在 MQTT 高级设置中选择协议（config.set 的 mqtt values.protocol）；省略该字段保持原模式。M1/A1 不提供协议切换。配置迁移不会擅自改变 TC1 已保存的模式。切换协议或账号会改变 HA 设备与实体标识，需要重新发现。

用户名大小写、空白原样保留，最多 63 个 UTF-8 字节；密码最多 256 个 UTF-8 字节。空白密码输入保留原密码，显式清除才删除。查询只返回密码是否已保存。

## TMCP（v2）主题

`user_key = u_ + hex_lower(UTF8(username))`；无用户名时为 `local`。MAC 使用 12 位小写十六进制。

设备根：`phicomm/v2/<user_key>/<tc1|m1|a1>/<mac>`。

| 后缀 | 用途 |
| --- | --- |
| `command` | 统一命令，不接受 retained 控制 |
| `result` | 命令响应，不保留 |
| `state` | 当前状态，retained |
| `capabilities` | 动作列表和协议元数据，retained |
| `availability` | online/offline，retained，LWT 为 offline |

HTTP 使用 `POST /api/v1/command`，与 MQTT command 共用 JSON 命令和响应。HTTP 路径中的 v1 是 HTTP 接口版本，不是 MQTT Legacy 版本。

请求：`{"request_id":"query-1","action":"sensors.average.get","params":{}}`。
响应：`{"request_id":"query-1","code":"ok","message":"ok","data":{...}}`。

状态、设置查询、能力查询中的 `mqtt_protocol` 为 `tmcp` 或 `legacy`，`mqtt_protocol_version` 为 2 或 1。历史 `mqtt_topic_mode` 字段保留，值为同样的小写协议名。能力中的 `protocol:2` 指统一命令协议版本；状态中的 `version` 仍是固件版本。

上线公告使用 `phicomm/v2/<user_key>/discovery/<mac>`，QoS 1、retained：

```json
{"schema":1,"model":"a1","protocol":"tmcp","protocol_version":2,"version":"v2"}
```

`model` 按实际型号变化；Legacy 为 `protocol:"legacy"`、`protocol_version:1`、`version:"v1"`。`schema:1` 是公告结构版本。

**平台配套要求：** 当前相邻仓库 `phicomm-community-mqtt` 的 M1/A1 校验仍要求 `version:"community"`，账号 ACL/业务主题仍使用旧根。平台必须同步支持 v2 元数据和 `phicomm/v2` 根，才能完成自动登记和授权。本次固件修改未部署或修改生产平台；不能据固件构建通过宣称现有平台可直接使用。

账号隔离需要 Broker ACL 实际限制对应 user_key；仅主题带用户名不构成授权。连接后先公告，再检查真实 SUBACK；授权不足会退避重试，不降级。HA 发现主题为 `homeassistant/<component>/<user_key>/<model>_<mac>_<entity>/config`，设备/实体 ID 包含账号、型号与 MAC。HA online 后重发发现配置。

绑定/地址等辅助根为 `phicomm/v2/<user_key>/<kind>/<id>`；点对点联动为 `phicomm/v2/<user_key>/<model>/<mac>/link/<peer>/<leaf>`。M1 联动仅响应配置允许的查询，不接受联动写操作。

## A1 实时平均值

`sensors.average.get` 仅由 A1 提供，params 必须为空。HTTP 和 MQTT 均可查询。`binding.status.get` 的 `average` 字段和联动虚拟设备的 `average` Topic 使用同一数据结构：

```json
{"valid":true,"sample_count":2,"configured_count":3,"temperature":25,"humidity":50,"pm25":20,"formaldehyde":0.1}
```

仅统计已绑定、数值合法且未过期的 M1；按四项分别计算算术平均值，不受自动联动配置的最大值/中值等策略影响。单位依次为 °C、%、µg/m³、mg/m³。没有有效采样时 `valid:false`、`sample_count:0`，四项值均为 null。查询时重新检查新鲜度；零值是有效测量。平均值不保留发布，失效时也发布无效结构，避免沿用旧值。

## 验证边界

`tests/run.sh` 验证协议、配置迁移、采样新鲜度、联动与命令行为；`tests/test_web.cjs` 验证离线页面和命令映射；`tests/build_host.sh`、`tests/test_mqtt_v2.py <隔离 Broker 端口>` 验证实际 SDK MQTT worker（外设为桩）。这些不能替代真实 HA 加载、Flash 迁移和物理设备验收。
