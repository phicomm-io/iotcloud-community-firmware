# MQTT 平台账号主题

TC1（A1 硬件版本）、M1 和 A1 可在离线网页的 MQTT 设置中选择主题模式：

- `legacy`：保留原主题；平台中需手动绑定。已有配置升级和恢复出厂设置均默认此模式。
- `account`：填写平台返回的实际 `mqtt_username` 和账号密码；上线后自动加入该账号的设备列表。TC1 使用 v2 文本协议。

用户名大小写和空白原样保留，最多 63 个 UTF-8 字节；密码最多 256 个 UTF-8 字节。密码留空保留已保存的密码，勾选“清除 MQTT 密码”才清除。读取设置只返回是否已保存 MQTT 密码，不返回密码。

## 主题与授权

`user_key = u_ + hex_lower(UTF8(username))`，MAC 为 12 位小写十六进制。原生业务根为 `phicomm/<tc1|m1|a1>/<user_key>/<mac>`。

每次 CONNECT 后首先以 QoS 1、retain=true 发布 `phicomm/discovery/<user_key>/<mac>`：TC1 的载荷为 `{"schema":1,"model":"tc1","version":"v2"}`；M1/A1 使用相应型号和 `version=community`。

只有 SDK 检查真实 SUBACK 授权后，才发布 HA 配置、完整当前状态及 retained `availability=online`。授权失败按 1、2、4、8、15、30 秒退避重连，每次重发声明；不会降级为旧主题。LWT 为同根 `availability=offline`，QoS 1、retain=true。

状态接口提供 `disconnected`、`connecting`、`config_error`、`auth_failed`、`waiting_acl`、`ready`。平台解绑后设备会等待权限，明确恢复后自动重试上线。普通授权等待不会删除 HA 配置。

HA 配置为标准五段 `homeassistant/<component>/<user_key>/<model>_<mac>_<entity>/config`，标识包含账号、型号、MAC 和实体。`homeassistant/status` 仅订阅读取，收到 `online` 后重发配置和有效状态。

所有控制均为绝对设置，订阅 QoS 1，拒绝 retained 命令。账号状态 retained；M1 只接受有效新采样，零值有效，超过 15 秒后传感器离线并清除 retained 测量。重连先初始化传感器离线，缓存采样不会重新续期。

绑定和组通信分别使用 `phicomm/bind/<user_key>/<目标MAC>`、`phicomm/address/<user_key>/<本机MAC>`、`phicomm/linkage/<user_key>/<原逻辑根>`。组 HA 标识由配置中的最小 MAC 固定，避免随成员在线状态变化；绑定令牌、LAN UDP、定时及本地控制保持原机制。

## 升级与切换

M1/A1 的旧 v1/v2 配置和 TC1 v10 配置迁移保留原设置，模式为 legacy。TC1 SDK 会按冻结的旧参数长度验证主/备份分区 CRC，再迁移新布局。

修改 Broker、账号、密码或模式会终止旧会话并清除旧发送及待回退请求。固件使用旧连接和旧凭据向旧 HA config 发送空 retained 消息，并尝试宣布旧设备离线；离线期间修改设置也会尝试使用保留的旧凭据清理。清理失败不阻塞保存，网页提示手动清理旧 Broker 中的 HA 实体。固件不会使用新账号删除其他账号主题，也不会用删除发现声明请求平台解绑。

## 可复现验证

```sh
# SDK 准备完成后：配置迁移、主题边界、绑定/组/UDP/RPC 及网页语法
./tests/run.sh
npm install --prefix build/ui-tests jsdom@30.1.2
NODE_PATH="$PWD/build/ui-tests/node_modules" node tests/test_web.cjs

# 三型号实际 MQTT worker + 各自实际 SDK，POSIX 网络和外设桩
./tests/build_host.sh
python3 -m venv build/account-host/venv
build/account-host/venv/bin/pip install paho-mqtt==2.1.0
build/account-host/venv/bin/python tests/test_broker.py --platform ../phicomm-community-mqtt

# 固件构建（串行）
./M1/build.sh
./A1/build.sh
./TC1/build.sh
```

Broker 测试使用临时目录、随机本机端口、隔离的 Mosquitto 2.0.22 动态安全实例及真实平台 Go 后端；不修改平台业务代码或生产服务。场景覆盖首次 SUBACK 拒绝、声明与自动登记、延迟授权、256 字节密码、三型号 HA 配置与状态、retained 控制拒绝、正常命令分发、跨账号订阅拒绝、断线 LWT、M1 零值与采样过期、解绑/恢复，以及切换账号时的旧 HA 清理。

上述测试验证协议和主机构建。外设由桩模拟，尚未进行真实 TC1 继电器/计量、M1 传感器或 A1 风机/RGB 的设备验收；参数迁移的实际 Flash 读写和真实 HA 重启也仍需实机验证。
