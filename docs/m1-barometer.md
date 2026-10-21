# M1 可选气压传感器

M1 可在现有 I²C1 总线上接入一颗 BMP388 或 BMP390，与 SCD40 共用
SDA（GPIO8）、SCL（GPIO7）和 GND。气压传感器不依赖是否安装 SCD40。

固件每次初始化依次检查 7 位地址 `0x76`、`0x77`，读取寄存器 `0x00`：

| 芯片 ID | 型号 |
| --- | --- |
| `0x50` | BMP388 |
| `0x60` | BMP390 |

此 ID 标识芯片型号，不是每颗器件唯一的序列号。不会按照模块商品名称猜测型号，
也不会向未知 ID 的器件写入传感器配置。如果两个地址均有支持的器件，使用首个成功初始化的器件。

七针模块的 SDI 接 SDA，SCL 接 SCL；I²C 模式下 CS 接模块的接口供电，
SDO 接 GND 选择 `0x76`，接接口供电选择 `0x77`；INT 可不接。
CS/SDO 的板载上下拉和 VCC 输入规格以实际模块说明为准。裸芯片 VDD 为
1.65–3.6 V，VDDIO 为 1.2–3.6 V，不能将未知模块直接按 5 V 模块使用。
参考 [Bosch BMP388 手册](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp388-ds001.pdf)。

未接入、读取失败或连续 10 秒没有新样本时，固件每 10 秒重试识别和初始化。
驱动和 SCD40 在同一硬件任务串行运行；不新增线程，不主动关闭共享 I²C 外设。
初始化使用官方软复位和出厂校准参数，支持 MCU 重启后传感器仍在测量的情况。

`status.get` 的 `full` 和 `live` 视图（HTTP、本地连接和 MQTT）都包含
`barometer` 对象：

```json
{
  "barometer": {
    "model": "BMP388",
    "chip_id": 80,
    "address": 119,
    "ready": true,
    "valid": true,
    "pressure_hpa": 1013.24,
    "age_ms": 100,
    "samples": 12,
    "errors": 0,
    "attempts": 1,
    "last_error": 0
  }
}
```

未识别时 `model`、`chip_id`、`address` 为 `null`；未取得样本时 `age_ms`
为 `null`。无效或样本达到 10 秒时 `pressure_hpa` 为 `null`，不把旧数据作为当前读数。
型号与地址在识别期间更新；失联后的诊断状态可能保留最后识别的型号，需同时检查 `ready/valid`。

配套前端在现有空气质量概览中显示一行 `气压 1013.2 hPa · BMP388`，
设备摘要显示有效 hPa；不新增气压卡片。未接入、旧固件、无效或过期时隐藏该行。
浏览器在轮询失败后也按样本年龄自行过期。内置页面需固件锁定对应前端制品。

数值为传感器测得的现场气压，不是海平面气压。采用原始出厂参数和官方补偿算法，
不添加人为偏移；本接入不自动将气压写入 SCD40，也不新增 HA/HomeKit 实体。
`valid` 仅表示通信和样本时效通过，不代表已经使用标准气压计校准。
