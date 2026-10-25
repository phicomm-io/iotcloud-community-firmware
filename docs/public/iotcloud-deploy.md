# IoTCloud 部署

直接使用现有平台管理设备，请先看[接入 IoTCloud](iotcloud-connect)。本页介绍自行部署与托管。

[打开本站 IoTCloud]({{SITE_URL}}iotcloud/) · [前端源码与发布](https://github.com/phicomm-io/iotcloud-community-frontend) · [后端与 Broker 部署](https://github.com/phicomm-io/iotcloud-community-mqtt#安装)

## 无需登录的本地直连

[打开本地设备管理]({{SITE_URL}}local/)，填写设备内网 IP 和网页管理密码，即可控制设备并配置 Wi-Fi、MQTT。无需账号、后端或 Broker；页面与完整管理平台风格一致。设备需要运行支持本地直连的新版固件，浏览器需要允许访问本地网络。网页密码只保留在当前页面会话，刷新后重新填写。

独立部署时，将前端发布包的 `local/` 整目录发布到静态服务器，默认跳过登录，不需要填写后端地址。普通平台登录前也可填写设备 IP；登录后可在“设置 → 设备管理 → 详情”配置各设备内网地址，并选择自动连接或始终 MQTT。自动连接会先验证设备身份和网页密码，探测失败时继续使用 MQTT。

## 选择部署方式

| 需求 | 入口 |
| --- | --- |
| 直接使用本站前端 | [打开 IoTCloud]({{SITE_URL}}iotcloud/)，在登录页的“服务器设置”选择自己的后端 |
| 只托管前端到 GitHub Pages 或其他静态服务 | [前端静态部署说明](https://github.com/phicomm-io/iotcloud-community-frontend#iotcloud-静态部署与-web-app) |
| 自建账号、设备管理和 MQTT Broker | [平台部署说明](https://github.com/phicomm-io/iotcloud-community-mqtt#安装) |
| 前端与后端使用不同域名 | [跨域后端配置](https://github.com/phicomm-io/iotcloud-community-mqtt#安装) |

## 静态前端

从前端仓库的 [Releases](https://github.com/phicomm-io/iotcloud-community-frontend/releases) 下载所需版本的 `frontend.tar.gz`，按发布中的 `SHA256SUMS` 校验，解压后将 `platform/` 的内容作为静态网站发布。GitHub Pages 的项目子路径也可使用。

在登录页展开“服务器设置”，填写后端 HTTPS 根地址（不带 `/api`）和 MQTT WebSocket 地址。也可在发布前编辑网站根目录的 `config.json`：

```json
{
  "apiOrigin": "https://api.example.com",
  "mqttWS": "wss://mqtt.example.com/mqtt"
}
```

配置中只放公开地址，不放账号、密码或令牌。浏览器保存的服务器设置优先于网站默认值。

## 配套后端与 MQTT

按[平台部署说明](https://github.com/phicomm-io/iotcloud-community-mqtt#安装)初始化后端和 Broker，再配置公网 HTTPS 与 WSS。静态托管只承载网页；账号服务、设备同步和 MQTT Broker 仍需要运行服务端。

前后端跨域时，在后端 `backend/config.yaml` 中允许前端的准确来源，例如使用本站前端时：

```yaml
allowed_origins:
  - https://phicomm-io.github.io
```

来源只包含协议和域名（有非默认端口时包含端口），不带 `/iotcloud/` 路径或末尾斜线；`public_url` 仍填写后端自己的公网地址。只添加自己信任的前端来源。

平台账号模式应连接与该后端配套的 Broker；连接其他 Broker 时使用访客模式。HTTPS 网页需要使用 WSS。设备固件中的 MQTT Host 填写设备可访问的 Broker 主机名和对应端口，不要填写网页地址或 WSS 路径。前端域名与 MQTT Host 不同时，页面会提醒在 IoT 设备本地网页的 MQTT 设置中填写正确的 Broker 主机名。

## 安装为 Web App

使用浏览器的“安装应用”或“添加到主屏幕”安装 IoTCloud。首次联网后会缓存应用界面；离线可打开页面，但登录、同步与设备控制仍需要网络。发现新版本后，按更新提示确认即可切换。

## 将平台账号填写到本地设备

在 IoTCloud 的“设置 → MQTT 连接”或“设备管理”中点击“填写到本地设备”，输入同一局域网内设备的 IP，并核对 Broker 主机名、设备 MQTT TCP 端口和账号密码。平台会在浏览器中新开设备本地页面；公网平台服务器不会访问你的内网。

设备需要安装支持此功能的新版 WebUI。设备页显示发起平台的来源，点击“接收并预填”后，配置才填入设备前端；检查并点击“保存”后才提交到设备。已有设备网页密码仍然有效，不会被绕过。设备不会读取或导出已保存的 MQTT 密码。

跳转 URL 的 fragment 仅携带 Broker 主机名、端口、用户名及一次性握手信息，不含密码；密码在用户点击接收后通过窗口通信传递。握手两分钟内有效，取消或过期后清除未保存的导入信息。仅向已核对的设备 IP 发送，并仅在可信局域网使用：HTTP 设备无法防止网络窃听或篡改。浏览器阻止弹窗、隔离窗口或设备版本不支持时，请手动填写，不会降级为 URL 携带密码。

首页 MQTT Host 提醒可点击“我知道了”关闭；设置中仍能查看设备接入地址及说明。设备 Broker 设置中的“打开 IoTCloud”只打开平台入口，不附带账号密码。

## China / US 独立节点

| 节点 | 网站 / API | 设备 MQTT | 浏览器 MQTT |
| --- | --- | --- | --- |
| China | https://mqtt.fjrcn.cn | mqtt.fjrcn.cn:1883 | wss://mqtt.fjrcn.cn/mqtt |
| US | https://mqtt.fjrcn.com | mqtt.fjrcn.com:1883 | wss://mqtt.fjrcn.com/mqtt |

在平台顶部选择节点。**两个节点的账号、密码、设备和数据不互通，需分别注册。** 切换会重新加载页面并使用目标节点自己的登录状态和 MQTT 凭据，不会迁移数据。同名账号也不会关联。设备须填写所选节点的 Broker 主机名与该节点账号。

自建多个节点时，每个节点独立初始化数据库、Broker 和 secrets，不复制其他节点的 `runtime/`。可信跨域前端可逐项加入 `allowed_origins`，例如 `https://phicomm-io.github.io`、`https://mqtt.fjrcn.cn` 和 `https://mqtt.fjrcn.com`；`public_url` 仍为当前节点自己的公网地址。
