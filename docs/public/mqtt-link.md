# MQTT 与联动

设备可以只在本局域网使用，也可以接入 MQTT，从而连接 Home Assistant 或 MQTT 远程集中管理平台。这一步完全可选，不做任何配置时设备功能不受影响。

## 选择 MQTT 模式

在设备网页的 MQTT 设置中填写 Broker 地址、端口、用户名和密码。三款设备的协议模式不同：

| 设备 | 支持的模式 |
| --- | --- |
| **TC1** | 保留 Legacy（v1）与 TMCP（v2）两种模式，首次配置默认 TMCP |
| **M1、A1** | 固定使用 TMCP（v2）主题，不提供 Legacy 模式选择 |

M1 和 A1 会把旧配置中的模式标记在启动时归一化为 TMCP，同时保留 Broker 地址、端口、用户名和密码。运行时不会依据旧标记切回旧主题。

设备主题为 `phicomm/v2/<scope>/<model>/<mac>/...`。非空用户名对应 `u_<UTF-8 字节的小写十六进制>`；空用户名使用 `local`，仍使用同一套 v2 协议。Home Assistant 发现配置发布到 `homeassistant/<component>/<scope>/<model>_<mac>_<entity>/config`。

> 固件主题前缀本身**不构成权限隔离**。TMCP 的账户隔离仍需 Broker 的 ACL 配合。

## 接入 Home Assistant

设备发布 Home Assistant 的 MQTT 发现配置，HA 会自动发现实体。M1 和 A1 可以只读订阅 Home Assistant 的启动通知，因此 HA 重启后刷新更快。

在 HA 中配置好 MQTT 集成并指向同一 Broker 后，设备的读数与开关会作为实体出现。

## A1 与 M1 联动

M1 只检测、上报并应答**已授权**的 A1，不主动控制净化器。规则、聚合、风机、灯环和组控制全部集中在 A1 端。

### 配置步骤

1. **在 A1 端**：添加 M1 的 MAC，按需填写 IP，并配置联动规则。每台 A1 最多使用四台 M1。
2. 两台设备连接同一 MQTT 服务和账号；Broker 需允许该账号向 M1 的 command 主题发送命令。
3. 确认两台设备局域网互通、平均读数有效，再启用联动。M1 无需填写配对标识，页面只显示当前 A1 会话。

IP 留空时，A1 通过局域网广播查找指定 MAC 的 M1；跨网段或广播受限时填写可达 IP。

### 运行方式

A1 通过 MQTT 给 M1 发送会话 token，随后每五秒通过 UDP 查询。正常读取期间不重复发送授权，也不经 MQTT 转发传感器数据。UDP 不通时按退避间隔重试；没有有效数据时执行 A1 的离线风速策略。

会话 token 只保存在 RAM。M1 最多同时保留四台 A1 的会话，闲置三十分钟后过期；重启后需 MQTT 重新授权，A1 会自动重试，失败期间授权最多每分钟发送一次。已建立且持续读取的会话在 MQTT 暂时断开后仍可使用。

A1 对十五秒内有效的 M1 数据计算实时算术平均；没有有效数据时读数为 `null`。联动开关、阈值、风速策略、防抖与最短切换间隔都在 A1 设置中。A1 组控制保持独立。

> **双方需要一起升级。** 旧的 M1 许可列表和联合配对主题不再使用。

## MQTT 远程集中管理平台

[接入 IotCloud：从注册到设备上线](iotcloud-connect) · [打开本站 IotCloud 设备管理]({{SITE_URL}}iotcloud/) · [部署自己的 IotCloud](iotcloud-deploy)

[phicomm-community-mqtt](https://github.com/phicomm-io/phicomm-community-mqtt) 提供账号登录、设备列表、MQTT 连接与 Legacy / TMCP 设备管理，支持批量管理设备。

- 网站与 MQTT 使用同一用户名；MQTT 初始使用注册密码，之后可在设置中独立修改。登录后自动连接 MQTT，断网后自动重连。
- 设备控制界面沿用本地 WebUI 的风格与操作习惯。
- 支持[将平台账号预填到本地设备](iotcloud-deploy#将平台账号填写到本地设备)，在设备页检查并保存后生效。
- **第三方 MQTT 模式与平台账号模式的设备列表、连接凭据相互独立。**
- 未登录时可进入访客模式，使用本地设备列表。

自建 MQTT 服务的账号主题、配置迁移和权限说明见平台的 [文档目录](https://github.com/phicomm-io/phicomm-community-mqtt#readme)。

可以先在[在线演示]({{SITE_URL}}#demos)中体验远程集中管理平台界面：演示全部使用模拟数据，不连接真实设备。

在平台顶部可选择 China（`mqtt.fjrcn.cn`）或 US（`mqtt.fjrcn.com`）。两个节点的账号、设备与数据独立，需分别注册；设备须连接对应节点的 Broker。切换节点不会迁移数据。
