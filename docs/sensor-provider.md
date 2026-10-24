# 通用环境数据提供者

设备角色与产品型号分离。消费者只绑定数据提供者的稳定身份，按字段、单位、有效性与新鲜度使用测量，不通过 `model=m1` 或 `model=daikin` 判断能否联动。

TMCP 账号命名空间内的共享数据主题：

```text
phicomm/v2/<user_key>/sensor/<device_id>/state
```

消息为非 retained、QoS1；device_id 为 12 位小写 MAC，必须与主题匹配。示例：

```json
{"schema":1,"device_id":"9c9c1ff76748","boot":123,"sequence":1,"sensors":{"pm25":{"value":12,"unit":"µg/m³","valid":true,"age_ms":0}}}
```

`boot` 为启动身份，`sequence` 为当前启动中严格递增的发布序号，均为非零 uint32。消费者拒绝重复/倒序序号及近期已退役的 boot，重复递送不得刷新数据时效。提供者应在五秒内更新一次；没有传感器数据也可发布更新的无效状态，不能把旧采样的 age_ms 重置为零。

每个字段独立声明。A1 当前识别 temperature（°C）、humidity（%）、pm25（µg/m³）、formaldehyde（mg/m³）、co2（ppm，独立有效性）。未提供、单位错误、无效、非数值、超出可用范围或年龄不小于 15 秒（CO₂ 为 20 秒）的字段不参加聚合。未知字段忽略，TVOC 暂不作为 A1 调速指标，未来控制策略可复用同一传输契约，无需按产品扩展路由。

A1 只接受明确配置的提供者身份、当前 MQTT 账号中的实时消息；拒绝 retained 快照，防止订阅时把旧值误当新采样。身份与权限依赖 MQTT 认证和 Broker ACL，同账号具有该发布权限的客户端可以提供数据；本消息不附带设备签名。生产 Broker 需要允许 `sensor/<mac>/state` 的对应发布/订阅权限。

M1 和 DAIKIN 均支持账号内的通用授权主题 `sensor/<device_id>/command` 与共享测量主题。A1 优先采用已授权、有效的 LAN 读取；LAN 不可用时使用同账号 MQTT 测量。授权会话和测量只在 RAM 中维护，授权成功本身不表示已经建立 LAN 连接。M1 保留 v0.9.21 的产品授权主题与旧版平面测量字段，A1 在通用授权无应答时回退旧协议；同时接受新的字段映射。通用输入不调用产品型号判断，不需要伪装 M1 或伪造 SCD40。UDP 会话细节见 [sensor-session.md](sensor-session.md)。

A1 对各字段分别聚合，不要求一个设备同时拥有四种数据。`sensors.average.get` 与 `binding.status.get.average` 返回独立可用性：

```json
{"valid":true,"sample_count":1,"configured_count":1,"temperature":null,"humidity":null,"pm25":12,"formaldehyde":null,"fields":{"temperature":false,"humidity":false,"pm25":true,"formaldehyde":false}}
```

调速所选字段必须全部可用，否则使用已配置的离线风速；不使用缺失字段的零值运算。数据灯效按字段判断有效性。前端隐藏不可用的调速/灯效数据来源选项；数据失效不删除持久化规则或阈值，恢复后重新显示。用户保存启用规则时必须选择当前可用的数据。

前端源码维护在相邻 `phicomm-community-frontend` 仓库。开发验证可显式使用：

```sh
PHICOMM_FRONTEND_SOURCE=/path/phicomm-community-frontend ./A1/build.sh
```

该选项在 manifest 中记录开发来源、commit、dirty 与 HTML gzip SHA-256。默认构建仍使用 `frontend.lock.json`；发布前应完成前端正式 Release 并更新锁，不能将开发覆盖包当作已发布的前端版本。
