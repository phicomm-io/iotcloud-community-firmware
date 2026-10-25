# M1 主控扩展接口

命令通过已有鉴权的 `POST /api/v1/command` 或 TMCP 命令主题调用，格式为 `{"action":"alarm.get","params":{}}`。完整能力列表见 `capabilities.get`。

## 两档阈值

先调用 `alarm.get` 获取 revision，再调用：

```json
{"action":"alarm.set","params":{"sensor":"hcho","yellow":100,"red":300,"revision":0}}
```

sensor 为 hcho / pm25，yellow 取值 0–65535，red 取值 1–65535，yellow 不大于 red。每次提交必须使用最新 revision；冲突返回 busy，保存失败回滚。两类配置独立持久化，原厂串口帧分别为 0x05 / 0x06，两个数值使用大端 16 位。UART 工作线程每秒同步，发送失败重试，重启后重发，升级期间暂停。

`alarm.get` 返回两类配置、revision、configured 和 uart_sent。`confirmation=uart_send_only` 表示仅确认串口发送成功；数值来自联网模块保存记录，并非主控读回。默认未配置，不主动覆盖原厂设置。原厂灯色渲染使用固定分档，不能据此认定设置已改变灯色或声音；开源主控将配置两档用于颜色和高档告警。

## 主控版本与蜂鸣器

```json
{"action":"mcu.version.get","params":{}}
{"action":"mcu.version.query","params":{}}
{"action":"buzzer.set","params":{"enabled":true,"duration_ms":500}}
{"action":"buzzer.set","params":{"enabled":false}}
{"action":"wake_alarm.start","params":{"duration_seconds":60}}
{"action":"wake_alarm.stop","params":{}}
{"action":"wake_alarm.get","params":{}}
```

0x11 查询版本，响应最多 16 个可打印字符，启动最多自动查询三次，收到后停止。version_received 区分未收到版本；扩展协议和能力来自实际 0x70 查询。主控网络升级和 CO₂ 显示详见 [配套接口](m1-co2-screen-20261025.md)。

0x0e 控制蜂鸣器，duration_ms 为 1–5000，默认 500；到期停止独立于命令 FIFO，发送失败会重试。状态表示请求，并非声学检测。wake_alarm 每两秒请求 300 ms 鸣叫，默认持续 60 秒，最大 180 秒，实体屏幕开关事件可停止。

## 负温度与健康标志

采样帧字节 4 为 1 表示负温度，01 05 32 解析为 −5.5°C。HTTP、MQTT 的 full / live 状态统一包含 sensor_health：pm25_raw 为字节 11，auxiliary_raw 为字节 12（含义未确认），climate_raw 为字节 13。pm25_valid 和 climate_valid 结合对应标志与 15 秒采样新鲜度；all_flags_ok 要求三个标志均非零。顶层 sensor_valid 继续表示采样通信新鲜度，原读数字段沿用 main 的缓存展示语义；消费者可按逐传感器健康字段判断故障。

配置版本为 7，旧配置迁移保留现有字段，追加 CO₂ 配置。回退旧固件前需考虑配置兼容性。
