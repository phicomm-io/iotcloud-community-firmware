# IotCloud 部署

[打开本站 IotCloud]({{SITE_URL}}iotcloud/) · [前端源码与发布](https://github.com/phicomm-io/phicomm-community-frontend) · [后端与 Broker 部署](https://github.com/phicomm-io/phicomm-community-mqtt#本地初始化)

## 选择部署方式

| 需求 | 入口 |
| --- | --- |
| 直接使用本站前端 | [打开 IotCloud]({{SITE_URL}}iotcloud/)，在登录页的“服务器设置”选择自己的后端 |
| 只托管前端到 GitHub Pages 或其他静态服务 | [前端静态部署说明](https://github.com/phicomm-io/phicomm-community-frontend#iotcloud-静态部署与-web-app) |
| 自建账号、设备管理和 MQTT Broker | [平台部署说明](https://github.com/phicomm-io/phicomm-community-mqtt#本地初始化) |
| 前端与后端使用不同域名 | [跨域后端配置](https://github.com/phicomm-io/phicomm-community-mqtt#独立静态前端iotcloud) |

## 静态前端

从前端仓库的 [Releases](https://github.com/phicomm-io/phicomm-community-frontend/releases) 下载所需版本的 `frontend.tar.gz`，按发布中的 `SHA256SUMS` 校验，解压后将 `platform/` 的内容作为静态网站发布。GitHub Pages 的项目子路径也可使用。

在登录页展开“服务器设置”，填写后端 HTTPS 根地址（不带 `/api`）和 MQTT WebSocket 地址。也可在发布前编辑网站根目录的 `config.json`：

```json
{
  "apiOrigin": "https://api.example.com",
  "mqttWS": "wss://mqtt.example.com/mqtt"
}
```

配置中只放公开地址，不放账号、密码或令牌。浏览器保存的服务器设置优先于网站默认值。

## 配套后端与 MQTT

按[平台部署说明](https://github.com/phicomm-io/phicomm-community-mqtt#本地初始化)初始化后端和 Broker，再配置公网 HTTPS 与 WSS。静态托管只承载网页；账号服务、设备同步和 MQTT Broker 仍需要运行服务端。

前后端跨域时，在后端 `backend/config.yaml` 中允许前端的准确来源，例如使用本站前端时：

```yaml
allowed_origins:
  - https://phicomm-io.github.io
```

来源只包含协议和域名（有非默认端口时包含端口），不带 `/iotcloud/` 路径或末尾斜线；`public_url` 仍填写后端自己的公网地址。只添加自己信任的前端来源。

平台账号模式应连接与该后端配套的 Broker；连接其他 Broker 时使用访客模式。HTTPS 网页需要使用 WSS。设备固件中的 MQTT Host 填写设备可访问的 Broker 主机名和对应端口，不要填写网页地址或 WSS 路径。前端域名与 MQTT Host 不同时，页面会提醒在 IoT 设备本地网页的 MQTT 设置中填写正确的 Broker 主机名。

## 安装为 Web App

使用浏览器的“安装应用”或“添加到主屏幕”安装 IotCloud。首次联网后会缓存应用界面；离线可打开页面，但登录、同步与设备控制仍需要网络。发现新版本后，按更新提示确认即可切换。
