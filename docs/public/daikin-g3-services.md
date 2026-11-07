# G3 天气、提醒与外部数据

使用[最新兼容固件]({{SITE_URL}}#releases)，在“设置 → 屏幕”分别配置天气、提醒和外部数据。布局与外观见[屏幕编排](daikin-g3-layout)，安装见[主指南](daikin-g3-guide)。联网服务由 ESP32 运行，屏幕 MCU 接收规范化数据后显示。

## 天气

| 提供商 | 配置 |
| --- | --- |
| Open-Meteo | 个人非商业使用无需 Key；填写经纬度和显示地点 |
| 和风天气 QWeather | 自己的 API Key 和以 `.qweatherapi.com` 结尾的项目 Host |
| 心知天气 Seniverse | 自己的 API Key，权限与套餐需支持所选接口 |
| OpenWeather | 自己的 API Key，使用公制单位 |

广东广州可填纬度 `23.1291`、经度 `113.2644`、标签“广州”，建议每 900 秒刷新；允许范围 300～86400 秒。城市标签只负责显示，实际查询位置以经纬度为准。Open-Meteo 一次请求获取当前与三天预报；选择免 Key 提供商会清除之前的密钥和自定义 Host。其他提供商不附赠或共用私有 Key。

“校验服务器证书”默认关闭，按设备保存，可明确开启。数据查询保留 HTTPS，证书开关不改变固件更新的签名和哈希检查。天气图标与描述由规范化天气代码生成，描述跟随屏幕中英文；城市中文需完整字库加载成功。

“已过期”表示缓存的时间有效性已超期，并不等于城市未识别。先检查启用状态、经纬度、Key/Host、设备时间、最近错误和重试倒计时，再手动刷新。请求失败保留上次内容；失败按 30、60、120、240、300 秒退避。天气 worker 不重叠，手动刷新也至少间隔 15 秒。

TLS 需要可逐字节访问的堆和连续块，不能仅看 ESP32 总空闲量。上传布局/资源、OTA 与天气并发受限；内存不足时退避重试，见[内存诊断](daikin-g3-diagnostics#esp32-内存与天气)。

## 提醒事项

最多保存 32 条定时规则和 32 条通知。规则支持 UTC 时间的一次性提醒、本地时分的每日提醒和每周提醒；星期一为位 0。时区使用与屏幕一致的固定分钟偏移，北京时间为 `480`。启动和大幅校时不会补发全部积压规则。

通知持久保存，标记已读后再更新屏幕。可设置遵从免打扰、总是唤醒或仅未读时唤醒，也可让单条规则继承全局策略。标题最多 64 个 UTF-8 字节，正文最多 127 个字节；中文按 UTF-8 字节计数。

## API 与外部统计

命令沿用认证后的 `POST /api/v1/command`，`Content-Type: application/msgpack`，信封为 `{request_id, action, params}`。下表花括号表示编码前的对象，不能直接当 JSON 发给此接口。MQTT/LAN 使用相同业务动作；具体支持范围先查询能力。

| 动作 | 参数示例或用途 |
| --- | --- |
| `screen.weather.get` / `.refresh` | 查询脱敏配置与状态 / 请求刷新 |
| `screen.reminders.configure` | `{timezone:480,wake:0}` |
| `screen.reminders.set` | `{id:1,kind:2,minute:540,days:31,wake:255,enabled:true,title:"通风",text:"打开窗户"}`；kind 0 一次、1 每日、2 每周 |
| `screen.reminders.push` | `{title:"来访",text:"有人按门铃",wake:0}`，返回通知 ID |
| `screen.reminders.ack` / `.delete` | `{id:...}`，分别标记通知已读 / 删除定时规则 |
| `screen.page.set` | `{direction:"next"}`、`{direction:"previous"}` 或 `{page:3}`，页码从 1 开始 |

外部数据可推送数值、文字与图表，包含来源、名称、单位和 TTL；不要把过期数据持续当作有效读数。图表最多三条序列、每条 32 点，缺测用 `null`，有限数值绝对值不超过 `1e9`，TTL 为 1～86400 秒。完整参数、分块布局、资源格式和请求确认语义见[服务协议](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/SERVICES.md)。

获取接口与认证示例见[设备接口与协议](device-api)。读取成功、ESP32 排队成功、MCU 应答和实际显示是不同阶段；有请求编号的动作应查询匹配结果。
