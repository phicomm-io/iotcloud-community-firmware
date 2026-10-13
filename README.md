# phicomm-community-mqtt

支持 [phicomm-community-firmware](https://github.com/FUjr/phicomm-community-firmware) 的 MQTT 服务与网页控制台。设备显示为 **tc1(a1)**、**m1**、**a1**。tc1(a1) 表示 TC1 的 A1 硬件版本，与 A1 净化器是不同型号；兼容原 TC1 v1 JSON 协议和社区 TC1 v2 协议。

## 账号与设备

- 网站登录和 MQTT 使用同一用户名、密码。可先注册，再绑定设备；支持开放注册、邮箱验证码注册及关闭注册。
- 登录后设备列表保存到服务器 SQLite，可跨浏览器恢复。云端仅保存 MAC、型号、协议版本、归属和同步状态，不保存备注、MQTT 明文密码或实时数据。
- 同一 MAC 全局只能属于一个账号。解绑完成后撤销权限并释放 MAC；解绑待同步期间不会允许他人绑定。
- 访客可连接任意 WebSocket MQTT 服务，本地列表、备注和连接配置继续保存在浏览器。登录不会自动上传本地设备；可明确点击“导入本地设备到账号”，不会覆盖云端列表。
- 管理员独立登录，可创建、重置或删除账号、管理设备归属及注册模式。原始 ACL 编辑接口已移除。

## 权限更新方式

使用 Mosquitto 2.0 的官方 **Dynamic Security** 插件，后端通过验证服务器证书的内部 TLS 连接发送管理命令，并等待带关联 ID 的实际响应。新增设备、解绑、改密即时更新权限，不需要重载容器。

后端使用独立 Broker 管理账号和 CA 公钥连接管理接口。普通账号默认拒绝所有主题，仅获得自己设备对应的主题权限，不能访问 `$CONTROL` 管理主题。M1/A1 可只读订阅 Home Assistant 启动通知。

操作失败不会当作成功：设备保留 `pending` 或 `deleting` 状态，每 10 秒重试；成功收到权限撤销响应后才释放 MAC。两份状态（SQLite 与插件 JSON）需要一起备份、恢复。不要多实例运行后端；当前同步锁及限流为单实例设计。`trust_proxy: true` 仅适用于后端只可由可信 Nginx 访问的部署，Nginx 会覆盖 `X-Real-IP`；直接开放后端时必须关闭该选项。

## 本地初始化

要求 Docker Compose、可用的 TCP 1883 和 HTTP 80 端口。首次运行：

```sh
mkdir -p runtime legacy
printf 'APP_UID=%s\nAPP_GID=%s\n' "$(id -u)" "$(id -g)" > .env
docker compose run --rm --build setup init --output /runtime
docker compose up -d --build mosquitto backend nginx
```

打开 `http://服务器地址/`。管理员用户名默认 `admin`，初始随机密码在 **runtime/secrets/admin-password**，请在本机私下查看。Broker 管理密码、密码哈希、证书私钥和数据库均在被 Git 忽略的 `runtime/` 中。初始化不会输出密码，也不会覆盖已有配置。若初始化中途失败，请保留错误信息，使用新的空目录重新初始化；不要将初始化命令用于正在运行的数据目录。

设备固件设置 MQTT 地址、1883 端口和自己的网站账号密码。网页默认连接同站点 `/mqtt` WebSocket；账号模式使用站点公布的 MQTT 地址。`.env` 可设 `HTTP_PORT`、`MQTT_PORT`、`PUBLIC_URL`、`MQTT_URI`、`MQTT_WS_URI`。自定义远端 WebSocket 地址必须指向同一套账号 Broker。

生产环境在前置代理终止 HTTPS，设置 `PUBLIC_URL=https://你的域名`，并将 `backend/config.yaml` 的 `cookie_secure` 设为 `true`。仅发布 HTTP 与设备所需 MQTT 端口；管理 TLS 8883、原始 WebSocket 9001 和后端 8080 不对外发布。1883 为兼容旧设备使用明文 TCP，请限制到可信设备网络。内部生成证书仅覆盖 `mosquitto`、`localhost`，服务器证书有效期两年；外部 MQTT TLS 需另行配置公网证书和监听器。

邮箱模式需配置 `backend/config.yaml` 的 SMTP 项，支持 TLS 直连或 STARTTLS。验证码仅驻留内存，有效期五分钟；重启后需要重发。

## 从旧 tc1_mqtt 迁移

迁移是离线工具，不读取或修改原服务，也不触发原容器重启：

1. 在原环境停止账号/设备修改，安全备份原 `passwd`、`acl`。不要把它们提交到 Git。
2. 将副本放入本项目 `legacy/passwd`、`legacy/acl`；使用新的空 `runtime/`。
3. 运行：

```sh
mkdir -p runtime legacy
printf 'APP_UID=%s\nAPP_GID=%s\n' "$(id -u)" "$(id -g)" > .env
docker compose run --rm --build setup migrate --output /runtime --passwd /legacy/passwd --acl /legacy/acl
```

4. 检查迁移账号数量、设备数量，再启动新服务。保留原服务与备份，切换地址前验证客户端登录及设备权限。

支持 Mosquitto `$7$` PBKDF2-SHA512 密码哈希，原密码无需重置；保留所有重复 `user` ACL 段中的设备，统一大小写和 MAC 分隔符，检测重复归属。未知规则、宽泛通配符、旧管理账号冲突或不支持的哈希会直接报错，需人工处理，避免默默丢弃权限。支持原 `device/ztc1/<MAC>/#`、`homeassistant/+/<MAC>/#` 及 M1/A1 的设备主题；TC1 迁移后默认 v2，可在网页调整为 v1。旧密码/ACL 的私有快照保存在 `runtime/backup/`。

浏览器旧 `ztc1_devices` 字符串数组和 `{mac,version}` 数组均可读取、按规范 MAC 去重。访客模式仍支持二维码和配置导入导出；导出的访客连接配置可能包含密码，应只在可信设备之间传递。

## 协议与界面

| 显示型号 | 协议 / 主题 | 控制与状态 |
| --- | --- | --- |
| tc1(a1) | v1 JSON 或 v2 `device/ztc1/<MAC>/set`、HA 状态 | 六路插座、总开关、童锁、指示灯及原 v1 功能 |
| m1 | `phicomm/m1/<小写无分隔符MAC>/...` | 温湿度、甲醛、PM2.5、0–4 屏幕亮度 |
| a1 | `phicomm/a1/<小写无分隔符MAC>/...` | 风扇开关/转速、RGB 灯光、童锁、指示灯常亮 |

显示名去掉 zTC 前缀，但 **TC1 原有 `ztc1` 线协议主题保持兼容**。v2 命令使用大写无分隔符 MAC。M1 传感器超过 15 秒没有新数据、收到过期通知或 MQTT 断线后不再显示为有效数据；亮度状态表示设备请求值，不是硬件执行确认。所有命令 `retain=false`，避免固件丢弃保留命令或重连执行旧命令。

## 验证与开发

```sh
(cd backend && go test -race ./... && go vet ./...)
(cd frontend && npm ci && npm test && npm run build)
./scripts/test-integration.sh
# 安装 Chromium 后验证完整 Compose 和网页流程
./scripts/test-browser-compose.sh
```

集成测试使用临时 Broker 和随机本机端口，结束后自动清理测试容器与数据。

浏览器测试需先启动一个独立后端和 Broker，开放注册，配置匹配的 `PUBLIC_URL`。前端开发服务支持 `API_TARGET` 和 `MQTT_TARGET`：

```sh
cd frontend
API_TARGET=http://127.0.0.1:8080 MQTT_TARGET=ws://127.0.0.1:9001 npm run dev -- --host 127.0.0.1 --port 25173
# 另一个终端
npx playwright install chromium
PHICOMM_WEB_URL=http://127.0.0.1:25173 PHICOMM_MQTT_WS=ws://127.0.0.1:9001 npx playwright test
```

浏览器测试会创建测试账号，请使用独立测试数据目录。`build.sh` 用于构建本地镜像。
