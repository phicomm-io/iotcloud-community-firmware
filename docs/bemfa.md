# TC1 / A1 巴法云接入

本分支在模块上独立连接原 MQTT 服务与巴法云；两个服务可以同时启用，也可以分别关闭。HTTP 本地控制持续可用，M1 保持原有接入方式。当前实现不经过 IotCloud 平台中转。

在巴法云的 [MQTT 控制台](https://cloud.bemfa.com/tcp/index.html)创建对应主题，从[连接密钥页面](https://cloud.bemfa.com/web/v2/index.html#/connection-secret/index)取得 AppID 和 SecretKey。在设备本地网页的 MQTT 设置中填写两项密钥及主题，再启用巴法云。使用的是连接密钥，不是巴法云登录密码。密钥输入框留空会保留已保存的值；勾选清除后保存会删除密钥，清除前需要关闭巴法云。读取配置只返回密钥是否已设置。

| 设备控制 | 主题后缀 | 下发消息 |
| --- | --- | --- |
| TC1 第 1–6 路插孔，各自一个主题 | `001` | `on` / `off` |
| A1 风机 | `003` | `off` / `on` / `on#1` / `on#2` / `on#3` / `on#4` |
| A1 灯环，独立主题 | `002` | `on` / `off` / `on#1` 至 `on#100`（亮度百分比） |

主题只能包含英文字母、数字和下划线，最长 63 字节；同一设备不能重复使用主题。留空可不映射该路，启用时至少保留一个主题。TC1 主题数组顺序对应网页第 1–6 路，API 内部插孔索引仍为 0–5。

A1 风机四档对应 **20 / 50 / 75 / 100%**，关闭对应 0；单独 `on` 恢复原有控制设置。调速保留当前自动／手动模式：手动模式调实际速度，自动模式调当前时段的速度上限，夜间时段只调夜间上限。状态按实际生效的速度映射到最近档位，中点取高档；自动运行速度为 0 时上报关闭。灯环遵循原有点亮及夜间策略，报告实际可见状态；本版本不增加 RGB 或摇头控制。

设备使用不同的客户端 ID，通过 `bemfa.com:9501` 连接，以 QoS1 订阅配置的原主题，以 QoS0 将状态报告到 `<主题>/up`，避免状态回流为控制指令。修改配置或断网后会重连；关闭巴法云后释放其连接和队列。原 MQTT 的认证和断线不影响巴法云，反之亦然。

小爱同学的绑定操作见[巴法云官方说明](https://cloud.bemfa.com/docs/src/speaker_mi.html)。个人开发者服务目前按[官方说明](https://cloud.bemfa.com/docs/src/business/user.html)免费；服务政策以巴法云为准。

API：`capabilities.get` 的 `dual_mqtt: true` 表示支持此配置。`config.set` 的 `mqtt` 段增加布尔值 `enabled`；`bemfa` 段支持 `enabled`、`app_id`、`secret_key`、`topics`（TC1 六项，A1 两项）。省略 `secret_key` 保留原值，空字符串清除。状态增加 `mqtt_enabled`、`bemfa_enabled`、`bemfa_connected`、`bemfa_status`、`bemfa_errors`、`bemfa_last_stage`、`bemfa_last_error`。两路开关和状态分别展示。

构建前安装 `requirements-build.txt`。完整页面注入 HTTP 队列脚本后使用 Zopfli 重新生成标准 gzip；设备端无需额外解压代码。前端源码与固件分开维护，按 `frontend.lock.json` 校验精确提交与发布包，未发布的本地包通过 `PHICOMM_FRONTEND_ARCHIVE` 指定。
