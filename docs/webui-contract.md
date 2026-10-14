# WebUI 设计与接口约定

TC1、A1、M1 的离线页面由 `common/web/page.py` 展开公共片段后压缩，浏览器无需 CDN。`style.css` 是唯一公共样式来源，`wifi.html`、`mqtt.html`、`auth.html` 是公共表单，`diagnostics.js` 是公共状态词表与诊断呈现。直接打开源码 HTML 不会展开片段；请使用构建产物或 `python3 common/web/page.py <页面路径>`。

## 页面与词表

- 标题显示设备类型与设备名称；一级导航为“首页”“设置”。首页展示设备的核心控制及测量值。
- 设置采用相同导航与卡片/表单：Wi-Fi、MQTT、设备、固件升级、诊断。设备专用功能采用相同组件和操作反馈。
- 固定使用“设备名称”“网络名称”“Broker 地址”“主题模式”“用户名”“密码”“访问认证”“网页密码”“童锁”“定时任务”“夜间模式”“按键事件”。网页认证用户名固定为 `admin`；MQTT 用户名独立可编辑。
- 状态值由公共词表翻译，分组诊断与原始 JSON 同时提供；用户输入通过 `textContent` 呈现。
- “请求已接受”表示接口接受，不等同于机械输出、物理按键或计量校准已验证。

## 公共 HTTP 定义

配置请求使用 `Content-Type: application/json`，成功响应包含 `message`。输入必须在完整接收、类型与长度校验后才生效。TC1 保留旧纯文本接口作为兼容入口，但新页面只发送 JSON（TC1 特有功率系数保留旧接口）。

| 方法与路径 | 请求/响应约定 |
| --- | --- |
| GET `/status` | `version`, `device_id`, `name`, `ssid`, `ip`, `wifi_connected`, `mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_topic_mode`, `mqtt_status`, `mqtt_connected`, `web_password_set`, `wifi_password_set`, `mqtt_password_set`, `ota_state`, `ota_error`, `ota_progress`；设备专用字段额外提供 |
| POST `/deviceName` | `{"name":"设备名称"}` |
| POST `/wifi/config` | `{"ssid":"网络名称","password":"","open_network":false}`；同一网络密码留空保留原值 |
| POST / GET `/wifi/scan` | 启动扫描 `{}`；查询 `{"scanning":false,"networks":[{"ssid":"...","rssi":-50,"security":...}]}` |
| POST `/mqtt/config` | `host`, `port`, `user`, `password`, `topic_mode` (`legacy`/`account`), `clear_password`；留空地址停用，留空密码保留，显式清除独立控制 |
| POST `/auth/password` | `{"password":"..."}`；8–32 字节可打印 ASCII，空密码关闭认证 |
| POST `/reboot` | `{}` |
| POST `/childLock` | `{"enabled":true}`（TC1、A1） |
| POST `/ota` | `{"url":"http://..."}`；签名 OTA 直链 |
| POST `/ota/fileUpload` | 原始字节流，强制签名验证；完成返回 HTTP 202，随后重启 |
| POST `/ota/fileUploadUnsigned` | 仅用户勾选“允许未签名的固件”后使用；raw 分支不加载签名验证上下文，仍检查设备镜像与写入结果 |
| GET / POST `/schedule/config` | `{"events":[...]}`；每周语义统一 `days` 位掩码（周一 bit0）、`minute`（当地时间分钟）、`on`（0/1）；设备动作字段按能力扩展 |

密码不通过状态接口返回。保留的 TC1 `station_pwd`、`ap_pwd`、`mqtt_pwd` 兼容键始终为空。

TC1 持久化结构保持原布局，因此 Broker 地址上限仍为 31 字节（A1/M1 为 127）；不通过改动 Flash 结构冒险扩大容量。TC1 名称按 UTF-8 字节限制为 63，插座名称相同。

## 能力区别

| 能力 | TC1 | A1 | M1 |
| --- | --- | --- | --- |
| 核心功能 | 六路插座、总开关、功率、电量 | 风机、RGB 灯环 | 空气参数、屏幕亮度 |
| MQTT、认证、重启、签名/raw OTA、状态诊断 | 支持 | 支持 | 支持 |
| 童锁 | 面板按键 | 面板电源/调速；配网键保留 | 不提供 |
| 定时任务 | 本机动作；8 个页面事件；一次任务扩展 `timestamp`，动作扩展 `operation` | 现有调度引擎 | 现有调度引擎 |
| 夜间模式 | 不提供 | 现有 `/sleep/config` | 现有 `/sleep/config` |
| 自定义按键事件 | 短按/长按映射，序号 1–30 | 保留现有固定面板行为 | 保留现有固定面板行为 |
| 文本系统日志 | 最近 64 条，每条 128 字节，GET `/log` | 现有串口日志与状态 | 现有串口日志与状态 |

TC1 每周任务按北京时间 UTC+8，网页与设备时间一致；未校时不能创建新页面事件。原 Flash 中最多 128 个旧任务仍可读取、执行和通过旧 `/task/<timestamp>` 删除。重复任务错过多个周期时直接推进至下一次未来执行，避免逐秒补执行。恢复出厂、配网和重启类动作保留旧能力，不在自动实机测试中执行恢复出厂。

## TC1 资源与兼容边界

- 固定日志环代替逐条 `malloc/free`，保留条数由 100 调整为 64；固定快照减少诊断读取时的分配。
- 遥测使用一次有界响应分配，按名称转义的最坏长度计算容量，避免 per-field JSON 对象堆；Wi-Fi 扫描最多保存 24 个网络，20 秒后退出扫描状态。
- 定时表在启动时从持久化值重建链表，忽略保存的指针；增删/替换/执行均使用锁，空表和容量上限有回归测试。
- 功率历史格式化有长度边界；长时间空载后的 IRQ 补记录循环最多处理环容量，计量系数与算法保持现有定义。
- 网络校时采用单调时钟安排重试，成功后 1 小时，失败后 60 秒，避免整点重复与快速重试。
- HTTP 与 MQTT 线程栈未凭推测缩小；没有物理按键、负载计量及长期耐久测试证据时，不宣称这些硬件边界已通过。

网页重启与旧 `soft` 请求均先发送响应，再通过 SDK 重启；软件重启不再把打包镜像头当向量表直接跳转。
