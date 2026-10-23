# 新风的 HA 与 HomeKit 归属

由独立子代理进行只读代码和官方资料调查，未操作真实 HA / Apple Home。

| 情况 | 当前结果 |
| --- | --- |
| A1 未绑定 M1/未分组 | 原风机、新风、转速传感器共用物理 A1 的 device.identifiers：一个 HA 设备，多个实体 |
| A1 绑定 M1 或分组 | 原风机切换为虚拟净化器，新风仍挂物理 A1：两个 HA 设备 |
| 两个 fan 经 HomeKit Bridge | 两个 HomeKit accessory；同一 HA 设备或同一 Bridge 不会自动合并 |
| RPM 反馈传感器 | HA 显示 RPM；原生 HomeKit Bridge 不支持通用 RPM sensor，diagnostic 实体也默认不导出 |

HA 可以通过让新风采用虚拟净化器的 device 标识，把两路风机归到同一设备，但必须保持每台物理 A1 的实体 ID、命令目标和 availability 独立，并处理分组迁移和多个新风实体。此次不擅自修改现有设备归属。

HomeKit Bridge 按 entity_id 分配 accessory ID，没有 linked_fan 配置。推荐分别暴露“空气净化器”和“新风风机”；如必须一个 accessory 包含两路独立速度控制，需要自定义桥接实现。

本次已修复两项直接相关问题：反馈传感器与其他实体使用一致的设备型号；HA 百分比命令包含 on（速度大于 0 为开启，否则关闭），避免 HomeKit 只发送速度时新风不能开机。通用 fresh_air.set 的独立开关语义保持不变。

官方资料：

- [MQTT Fan 设备标识](https://www.home-assistant.io/integrations/fan.mqtt/#device)
- [HomeKit accessory ID](https://www.home-assistant.io/integrations/homekit/#accessory-id)
- [HomeKit 配置与支持类型](https://www.home-assistant.io/integrations/homekit/#configuration-variables)
- [HA 2026.10.2 按实体创建 accessory](https://github.com/home-assistant/core/blob/8d7fd8b68677c2585be36ec86043308f403875b8/homeassistant/components/homekit/__init__.py#L753)
- [HomeKit fan 写入分派](https://github.com/home-assistant/core/blob/8d7fd8b68677c2585be36ec86043308f403875b8/homeassistant/components/homekit/type_fans.py#L174)
- [MQTT fan 百分比发送](https://github.com/home-assistant/core/blob/8d7fd8b68677c2585be36ec86043308f403875b8/homeassistant/components/mqtt/fan.py#L509)
