# 传感器数据与 A1 联动

当前用户配置步骤以[MQTT 与联动](../docs/public/mqtt-link.md)、[A1 操作](../docs/public/a1-guide.md)及[DAIKIN 指南](../docs/public/daikin-guide.md)为准。原先双端配对、八台设备和联合主题的方案已移除。

A1 最多添加四个数据源，字段能力独立于型号。数据源通过同账号 `phicomm/v2/<scope>/sensor/<mac>/state` 发布非 retained 测量，通过对应 `command` 接收 `sensor.session.set` 临时授权。M1 保留旧会话兼容。UDP 端口为 11288，A1 优先读取局域网，通用 MQTT 测量可用于回退。

每字段校验单位、有效标志及年龄；PM2.5、甲醛、温湿度十五秒过期，CO₂ 二十秒过期。重复启动标识与序号不会续期。聚合按字段进行，缺失为 `null`，不作为零值。PM10、TVOC 由检测仪提供显示，当前不参与 A1 风速规则。失效字段的选项隐藏，已保存规则保留；控制所需数据不可用时使用离线风速。

会话 token 仅存 RAM，每台检测仪最多四台 A1，三十分钟闲置过期。只有实际读取成功才显示已连接；重启后通过 MQTT 自动重新授权。主题前缀不代替 Broker ACL。

实现入口：`runtime.c` 处理会话、传输与样本，`engine.c` 聚合并执行策略，`sensor_values.h` 定义字段能力；设备通过 TMCP `binding.config.get/set`、`binding.status.get`、`linkage.config.get/set` 管理联动。接口以各设备 `capabilities.get` 返回的动作及 [WebUI 契约](../docs/webui-contract.md)为准。
