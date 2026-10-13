# MQTT 账号主题协议 v1

本协议用于 phicomm-community-mqtt 与 phicomm-community-firmware 对接。平台已实现；固件实现要求见固件仓库 `doc/mqtt-account-namespace-requirements.md`。

## 身份与兼容

- 新主题的身份键为 `(MQTT 用户名, MAC)`，不全局占用 MAC；同一 MAC 可以同时属于不同账号的新主题，也可与旧主题记录并存。
- 旧纯 MAC 主题继续手动绑定，全局先到先得；发布旧状态或旧 HA 配置不会自动登记，也不会获得新主题权限。
- 型号是 `tc1`、`m1`、`a1`。TC1 展示名 `tc1(a1)`；新账号主题 TC1 仅支持 v2，M1/A1 为 community。
- 用户名为真实 MQTT 鉴权用户名，区分大小写。编码 `user_key = "u_" + hex_lower(UTF8(username))`，不 trim、不转小写、不使用 URL/base64 编码。邮箱注册模式使用注册返回的规范用户名。例：`alice` → `u_616c696365`；`a+b@example.com` → `u_612b62406578616d706c652e636f6d`。
- MAC 固定为 12 位小写十六进制，不带分隔符；账号主题中的 MAC 必须规范，命令载荷中 TC1 MAC 仍按旧 v2 为大写。

## 平台自动发现

固件使用自己的账号发布 QoS 1、retain=true：

```text
phicomm/discovery/<user_key>/<mac>
{"schema":1,"model":"m1","version":"community"}
```

TC1 使用 `{"schema":1,"model":"tc1","version":"v2"}`，A1 使用 `{"schema":1,"model":"a1","version":"community"}`。声明不超过 4096 字节，型号必填。平台从主题解析归属，不信任载荷中的用户名或 MAC。首次登记后型号/版本不能通过声明改变。

账号建立后只获得自己声明与 HA 命名空间权限及 `homeassistant/status` 只读权限；没有旧 MAC 权限。平台收到合法声明后将设备以 pending 保存到 SQLite，在线创建/绑定该设备的精确业务主题 ACL，成功后 active。用户网页每 3 秒自动同步云端列表，无需连接网页 MQTT 或手动添加。发现失败保留 pending，每 10 秒重试。

MQTT PUBACK 仅代表 Broker 接收声明，不代表平台已经授予业务权限。固件必须先发声明，再订阅业务命令；SUBACK 拒绝或发布/连接失败时重连、重发声明并重试，不能降级为旧主题。管理客户端重连会重新订阅 retained 声明，平台处理幂等。

## 业务主题

```text
phicomm/<model>/<user_key>/<mac>/<leaf>
```

M1/A1 的 leaf 和载荷保持现有协议；只增加用户名路径段：

| 型号 | 状态 | 命令 |
| --- | --- | --- |
| m1 | availability、sensor_availability、sensor JSON、brightness/state | brightness/set：0–4 |
| a1 | availability、fan/state、fan/percentage/state、light/state JSON、child_lock/state、led_always_on/state | fan/set、fan/percentage/set、light/set、child_lock/set、led_always_on/set |
| tc1 | availability；sensor JSON；socket_0/state 至 socket_5/state；total_socket/state、led/state、childLock/state | set：原 v2 文本命令，MAC 大写 |

TC1 `sensor` 采用现有 v2 JSON 字段，合并功率、电量、运行时间等信息；开关状态载荷保持 `set socket AABBCCDDEEFF 0 1` / `set total_socket AABBCCDDEEFF 1` / `set led ...` / `set childLock ...`。

状态与 availability retained；availability payload 为 online/offline，LWT 使用相同账号主题、QoS 1、retain=true、offline。M1 的传感器更新/过期规则保持 15 秒。所有控制命令 QoS 1、retain=false；固件不得执行 retained 控制命令。

## Home Assistant 自动发现

保持 HA 标准结构，不能额外增加一层用户名：

```text
homeassistant/<component>/<user_key>/<model>_<mac>_<entity>/config
```

`node_id` 为 user_key，object_id 为型号、MAC、实体键，保证同一账号多设备以及多账号同一 MAC 不冲突。每个实体 config QoS 1、retain=true。其 state_topic、command_topic、availability_topic 必须指向自己的账号业务主题；不要继续指向旧 HA 状态或旧纯 MAC 命令。

`unique_id` 必须包含 user_key、model、mac、entity；同一设备所有实体的 `device.identifiers` 使用 `<user_key>_<model>_<mac>`。HA 自动发现只读取配置；平台设备登记使用独立声明，不解析任意 HA 配置内容。平台的 HA 权限仅覆盖自身 user_key，设备业务权限按 MAC 精确发放。

示例：

```text
homeassistant/sensor/u_616c696365/m1_aabbccddeeff_temperature/config
```

```json
{
  "name":"温度",
  "unique_id":"u_616c696365_m1_aabbccddeeff_temperature",
  "state_topic":"phicomm/m1/u_616c696365/aabbccddeeff/sensor",
  "value_template":"{{ value_json.temperature }}",
  "unit_of_measurement":"°C",
  "device_class":"temperature",
  "availability_topic":"phicomm/m1/u_616c696365/aabbccddeeff/availability",
  "device":{"identifiers":["u_616c696365_m1_aabbccddeeff"],"name":"m1","model":"m1","manufacturer":"Phicomm"}
}
```

HA 启动通知 `homeassistant/status=online` 应触发重新发布 retained 发现配置及状态，不能向该共享主题发布。参照 [Home Assistant 官方 MQTT Discovery 文档](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)。HA 必须使用拥有这些账号主题权限的 MQTT 凭据；本平台不提供读取所有账号设备的共享 HA 账号。

## 解绑与恢复

- 新设备 DELETE `/api/devices/scoped/<mac>`：撤销精确业务 ACL，保存 hidden 墓碑。旧 retained 声明/固件重连不会立即重新登记。账号的声明及 HA 命名空间权限仍保留，因而不会影响同账号其他设备。
- 页面“恢复已解绑的账号主题设备”或 PUT `/api/devices/scoped/<mac>`：仅恢复 hidden 记录并重新同步 ACL，不接受修改归属或型号。
- 旧设备继续使用 `/api/devices/<mac>`；成功解绑后释放全局 MAC。
- 自己的列表返回账号设备 `namespace:"account"`、username、mac、model、version、status；旧记录 namespace 缺省。界面/存储键必须包含 namespace 与 username，不能只用 MAC。
- 管理员删除账号设备：DELETE `/api/admin/device/scoped/<mac>?username=<URL编码用户名>`。删除账号撤销其全部角色、会话和云端记录，不影响另一个账号同 MAC 的新设备。
- 平台解绑不会清理 HA 已接收的 retained 配置；固件切换模式、账号或删除实体时须用原凭据对旧 config 发空 retained 消息。关闭设备或正确清理 availability 后 HA 才显示实际离线状态。

## 已有安装升级

启动时新增 scoped_devices 表，不重建旧 devices 表，不变更旧归属、密码或会话；在线扩展 manager 发现订阅权限，为现有账号补充账号角色。不要重新运行 init 或编辑插件 JSON。备份 SQLite 与 Broker 数据后更新服务镜像即可，权限变更通过 Dynamic Security 管理协议完成，无 Docker socket 或容器重载。
