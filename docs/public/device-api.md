# 设备接口与协议

本文以 **v1.0.0-rc.6（Beta）** 与 **v0.9.27（稳定版）** 为接入基线。先在设备网页确认型号、固件版本和变体，再选择编码与能力；M1 传感器主控有独立版本。

## 基线与编码

| 项目 | RC6 | 0.9.27 |
| --- | --- | --- |
| 设备 | TC1、M1、A1、DAIKIN、ORVIBO S30-V1B | TC1、M1、A1、DAIKIN |
| HTTP 普通命令 | MessagePack，`application/msgpack` | JSON，`application/json` |
| MQTT 设备业务 | MessagePack，TMCP v2 信封 | JSON；TC1 可选 Legacy v1 或 TMCP v2，其余为 TMCP v2 |
| 传感器字段 | schema 3，描述与实时快照分开 | 按该版传感器 / 联动接口使用，不套用 schema 3 |
| M1 用户显示配置 | `display_profile.get/set`，GET schema 4 | 使用该版 CO₂ 显示与主控能力，不提供 profile4 |
| Home Assistant | 独立 JSON 发现 / 状态 / 控制主题 | 独立 JSON 发现 / 状态 / 控制主题 |

项目名为 `iotcloud`，设备协议名为 `openiot`；TMCP 是命令封装的内部标识。HTTP 路径 `/api/v1`、TMCP `protocol_version:2`、二进制 `wire_version:1`、传感器 schema 和 MCU UART 版本各自独立。此处的 proto 指协议定义；设备接口不使用 Protobuf `.proto`。

S30-V1B 还声明 CBOR 支持；跨设备示例统一使用默认 MessagePack。

RC6 不通过 JSON 请求、超时或 `busy` 自动降级。MQTT 的 `availability` 仍是文本 `online` / `offline`，固件上传和配置备份仍是各自的二进制文件流。

## HTTP 接入

未设置管理密码时直接访问，不建立登录会话。设置后可使用 Basic 认证，用户名固定为 `admin`，密码为设备网页密码。MQTT Broker 凭据和 UDP 配对凭据分别管理。在可信局域网使用 HTTP；浏览器跨域还需通过设备的 Origin / OPTIONS 检查与浏览器本地网络权限。

RC6 常用路径如下。普通 GET 查询返回数据对象；POST 命令返回带 `code/data` 的信封。具体动作以 `capabilities.get` 的 `data.actions` 为准，表中专用接口还需满足机型和变体条件。

| 方法与路径 | 返回或用途 |
| --- | --- |
| `GET /api/v1/info` | 型号、设备 ID、固件版本与编码标识 |
| `GET /api/v1/status` | 设备状态快照 |
| `GET /api/v1/capabilities` | 动作、容量与可选功能 |
| `GET /api/v1/config` | 脱敏配置，不返回密码 |
| `POST /api/v1/command` | 查询、控制与配置命令 |
| `GET /api/v1/memory` | 内存与通信诊断；按设备实现提供 |
| `GET /api/v1/config/backup` | 完整二进制备份，需已设置管理密码；妥善保存其中凭据 |
| `POST /api/v1/config/restore` | 上传匹配型号 / 布局的备份，接受后重启 |
| `POST /api/v1/ota/upload` | TC1 / M1 / A1 / DAIKIN 签名文件流 |
| `POST /api/v1/mcu/exchange` | M1 原始 MCU 帧桥接，独立于 MessagePack 命令 |
| `POST /api/v1/ota/convert` | 声明转换能力的 DAIKIN `full → hub`；按[中枢教程](daikin-guide#普通版切换为中枢)操作 |

S30-V1B 通过 `ota.start` 使用本设备 `.rbl` URL，不提供本地文件上传、配置备份恢复或 MCU 桥接。其他设备的包名和上传流程见[固件更新](firmware-update)。

下列字典是解码后的结构，RC6 在线发送的是 MessagePack 字节：

```python
request = {"request_id": "read_001", "action": "status.get", "params": {}}
response = {"request_id": "read_001", "code": "ok", "message": "操作成功", "data": {}}
```

按 `code` 判断结果：`ok` 为查询成功，`accepted` 为操作已接受；`invalid_params`、`unsupported`、`busy`、`conflict` 和 `storage_error` 分别处理。`message` 用于展示。写操作使用唯一 `request_id`；相同 ID 不得改写内容，回执去重只在设备有限缓存内生效。超时后先查询实际状态，不自动重发开关、重置或升级命令。HTTP 过载可能返回 JSON `busy`，也可能在形成 HTTP 应答前断开连接。

## Python 查询示例

下载[查询脚本](assets/examples/device_query.py)。它只提供能力、状态和脱敏配置查询，无自动重试，密码通过终端输入。RC6 需要 Python `msgpack` 包；0.9.27 只用标准库。

```sh
python3 device_query.py http://192.168.1.100 --baseline rc5
python3 device_query.py http://192.168.1.100 --baseline rc5 --action status.get --password
python3 device_query.py http://192.168.1.100 --baseline 0.9.27 --action status.get
```

脚本验证 HTTP 状态、响应编码、结果码及 `request_id`。打印的 JSON 仅用于终端阅读。

已有 `msgpack` 的 Python 环境也可生成请求文件供 curl 使用：

```sh
python3 - <<'PY'
from pathlib import Path
import msgpack
request = {"request_id": "caps_001", "action": "capabilities.get", "params": {}}
Path("request.msgpack").write_bytes(msgpack.packb(request, use_bin_type=True))
PY
curl --fail-with-body --max-time 10 \
  -H 'Content-Type: application/msgpack' -H 'Accept: application/msgpack' \
  --data-binary @request.msgpack http://192.168.1.100/api/v1/command -o response.msgpack
python3 - <<'PY'
from pathlib import Path
import msgpack
print(msgpack.unpackb(Path("response.msgpack").read_bytes(), raw=False))
PY
```

已设密码时，curl 加 `--user admin` 后按提示输入。0.9.27 可直接发送 JSON：

```sh
curl --fail-with-body --max-time 10 -H 'Content-Type: application/json' \
  --data '{"request_id":"caps_001","action":"capabilities.get","params":{}}' \
  http://192.168.1.100/api/v1/command
```

## MQTT 命令与发现

RC6 的设备根为 `phicomm/v2/<scope>/<model>/<device_id>`。`scope` 为 `u_` 加用户名 UTF-8 字节的小写十六进制，无用户名为 `local`；设备 ID 为原厂 MAC 的 12 位小写十六进制。型号段按公告读取，例如 `tc1`、`m1`、`a1`、`daikin`、`s30-v1b`，不要用芯片名称推导。

| 主题 | Retain | RC6 载荷 |
| --- | --- | --- |
| `phicomm/v2/<scope>/discovery/<device_id>` | 是 | MessagePack 公告，schema 2，含 `wire_version:1`、`wire_formats:["msgpack"]`、`default_wire_format:"msgpack"` |
| `<root>/availability` | 是 / LWT | 文本 `online` / `offline` |
| `<root>/capabilities`、`<root>/state` | 是 | MessagePack 能力 / 状态 |
| `<root>/command` | 否 | 与 HTTP 相同的 MessagePack 请求 |
| `<root>/result` | 否 | 按 `request_id` 匹配的 MessagePack 回执 |

先订阅 result，再发送命令，避免漏掉快速应答。以下仅查询能力；需按 Broker 要求补充认证和 TLS 参数：

```sh
mosquitto_sub -h BROKER -t 'phicomm/v2/local/tc1/001122334455/result' -C 1 -N > result.msgpack
# 在另一个终端发送前文生成的请求；不要加 -r。
mosquitto_pub -h BROKER -t 'phicomm/v2/local/tc1/001122334455/command' -f request.msgpack
```

Home Assistant 使用 `homeassistant/.../config` 及设备的 HA JSON 主题，不将普通 MessagePack 结果直接交给 HA JSON 模板。Broker 必须配置账号 ACL，不能只依赖主题前缀隔离账号。

## 传感器字段协议

RC6 schema 3 使用两个 MessagePack 主题：

- `phicomm/v2/<scope>/sensor/<device_id>/metadata`：保留字段描述。
- `phicomm/v2/<scope>/sensor/<device_id>/state`：非保留的完整实时快照。

```python
metadata = {
    "schema": 3, "device_id": "001122334455", "boot": 123,
    "descriptor_revision": 1,
    "fields": {"power": {"name": "功率", "type": "number", "unit": "W"}},
}
state = {
    "schema": 3, "device_id": "001122334455", "boot": 123,
    "descriptor_revision": 1, "sequence": 7,
    "sensors": {"power": {"valid": True, "value": 12.5, "age_ms": 0}},
}
# 两者分别使用 msgpack.packb(..., use_bin_type=True) 编码。
```

`boot`、`descriptor_revision`、`sequence` 为非零 uint32。描述与快照的设备、boot、描述修订必须一致。绑定使用 `device_id + field_id + type + unit`；名称仅展示。新快照撤销缺失或无效字段的旧值；失效值为 `valid:false/value:null`，不能用零冒充。重复、倒序、retained state 和未知描述不能续期。源年龄加本地经过时间还须满足该字段有效期和显示 TTL，通用数值上限为 20 秒；A1 的部分环境字段为 15 秒。M1 的物理三位屏、显示格式与主控能力另行约束数值范围。

## UDP 与 MCU proto

| 协议 | RC6 定义 | 入口 |
| --- | --- | --- |
| 传感器 LAN v3 | MessagePack 的 `read/probe/sample/pong`，16 字节 bin token；固定单位的整数测量 | `common/lan_wire.h`、`common/lan_wire.inc` |
| 业务 LAN v4 | UDP 11288，`_openiot._udp.local.`；`command/result/pull/ack`，内层命令是 MessagePack bin | `common/lan_wire_v4.h`、`common/lan_wire_v4.inc` |
| M1 UART | 原始串口帧、操作码与能力协商；`/mcu/exchange` 原样桥接 | `M1/protocol.h`、[MCU 升级教程](m1-mcu-update) |

LAN v4 单帧最多 1200 字节，命令体最多 1024 字节，响应最多 8192 字节，以 768 字节分片，缓存 5 秒。token 是 16 字节二进制值，不是十六进制文本；客户端必须先按配对 / 会话授权流程取得 token，管理密码为空也不能跳过 UDP 鉴权。丢失分片用 pull 获取，完成后 ack；不要通过再次执行写命令来取回结果。

## 实现与规范来源

本页 RC6 内容按[固定源码 b2477120](https://github.com/phicomm-io/iotcloud-community-firmware/tree/b2477120df5fa0b53a4bcb4cae480680bd66c460)核对：`common/service_mp.inc`（命令）、`common/service_product_mp.inc`（能力 / 身份）、`common/mqtt/discovery_mp.h`（公告）、`common/sensor_mp.h`（schema 3）、`common/lan_wire*.inc`（UDP）。0.9.27 参照[稳定版源码](https://github.com/phicomm-io/phicomm-community-firmware/tree/v0.9.27)。

[开发协议仓库](https://github.com/phicomm-io/iotcloud-community-protocols)提供协议索引、Schema、测试向量与生成类型。消费版本由 `protocols.lock.json` 固定；其中的历史 JSON 合同不能代替 RC6 当前二进制实现。修改接口时同时维护协议定义、正反样例、生成物和消费锁文件。
