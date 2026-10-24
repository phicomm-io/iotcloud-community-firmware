# A1 → 数据提供者的传感器会话 v2

A1 单方面配置提供者 MAC 与可选 IP。M1 和 DAIKIN 使用相同的运行时会话协议，不按型号选择数据能力。M1 原 Flash 许可字段保留布局但不参与授权；不会为会话写 Flash。A1 组控制协议不变。

A1 经同账号 `phicomm/v2/<user_key>/sensor/<device_id>/command` 主题发送：

```json
{"action":"sensor.session.set","params":{"v":2,"a1":"aabbccddee01","boot":123,"token":"32 hex characters"}}
```

该动作仅由 MQTT 命令入口处理，拒绝保留消息；HTTP、UDP 不能创建会话。提供者使用原有 result 返回执行结果，A1 无需订阅 result：匹配 token、目标和请求号的 UDP sample/pong 即证明授权可用。token 和启动标识不经状态查询返回；token 不进命令回执缓存。

token 每个运行时会话随机生成，账号切换清空会话，普通重连保留。同一 A1 的新授权替换旧值，重复授权幂等。提供者上限四会话，满额返回 busy，不驱逐活跃会话；闲置三十分钟后可回收。只有有效的 UDP 读取才计入网页的已连接 A1，授权成功本身不计入。

UDP v2 包含 src/dst、boot、id、op、token；read 返回 sample，probe 返回 pong。保留原样本时间、序号和启动 ID 防止旧数据刷新新鲜度。有效数据十五秒过期，CO₂ 二十秒过期。token 作为持有凭据，依赖受信任局域网与 Broker ACL，不提供局域网数据加密。

A1 正常每五秒读取，不重复授权。超时后重试等待为 1/5/15/60 秒，授权发送最多每分钟一次；初始入队失败五秒后重试。显式 IP 优先；否则尝试已学习地址，连续失败后广播查找。关闭恢复会停止失败后的 UDP 重试。提供者重启后，A1 在下一轮退避与授权重发后恢复；已经持续读取的会话不依赖 Broker 在线。

A1 优先使用有效的 LAN 数据，LAN 不可用时使用通用 `sensor/<device_id>/state` MQTT 数据。M1 同时发布旧版平面字段和新字段映射；通用授权无响应时，A1 会尝试旧版 M1 产品命令主题，兼容已安装的会话 v2 固件。更早的旧绑定握手仍需两端升级。
