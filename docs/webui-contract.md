# WebUI 设计与接口约定

当前基线为 **v1.0.0-rc.5**，稳定版接口基线为 **v0.9.27**。中英文的编码、主题、HTTP 路由、UDP 与 MCU 协议说明及可运行示例见[设备接口与协议](public/device-api.md)（[English](public/en/device-api.md)）。

## 公共前端

TC1、M1、A1、DAIKIN 与 ORVIBO S30M-V1B 采用公共前端的样式、布局和导航，仅显示已实现的硬件能力。首页展示核心控制与测量，设置按网络、设备、自动化和维护组织。设备名称、型号和身份分别显示，不使用芯片名代替整机型号。

`frontend.lock.json` 固定前端 Release、完整 commit 和 SHA-256；`tools/frontend_release.py` 校验下载，`tools/pack_frontend.py` 嵌入确定性 gzip 资源，离线页面不依赖 CDN。S30M-V1B 的公共资源来源及适配校验见 `BL2028N/web/shared/provenance.json` 和该产品 README。

## HTTP 与鉴权

RC5 普通业务使用 MessagePack，MIME 为 `application/msgpack`；0.9.27 使用 JSON，MIME 为 `application/json`。两者的命令路径均为 `POST /api/v1/command`，请求解码后为 `{request_id?,action,params}`，响应为 `{request_id?,code,message,data}`。不要把调试终端打印的 JSON 当成 RC5 线上编码。

未设置网页密码时直接访问，不占用会话；设置后支持 `admin` Basic，使用 token 的前端复用有效会话，退出、过期或密码变更后重新认证。密码查询只返回设置标记，表单省略密码表示保留，显式空字符串表示清除。MQTT / UDP 的授权独立于网页密码。

HTTP 查询路径、签名 OTA、备份和主控桥接见[接口表](public/device-api.md#http-接入)。备份及固件上传使用各自二进制文件格式；S30M-V1B 只提供 `.rbl` URL OTA，不显示未实现的上传或备份功能。

## 能力与动作

先调用 `capabilities.get`，按 `data.actions` 及容量字段创建控件。M1 的 Wi-Fi 版本与传感器主控版本分别读取；主控能力不足时隐藏或禁用相关操作。

| 动作 | 用途与条件 |
| --- | --- |
| `status.get`、`config.get`、`capabilities.get` | 状态、脱敏配置、能力 |
| `config.set` | `{section,values}`；按设备能力选择配置段 |
| `wifi.scan`、`wifi.scan.get` | 发起扫描与读取结果 |
| `logs.get` | `{after,limit}`；最多 8 条 / 页，`records` 与 `next/latest` 游标 |
| `schedules.get/put/delete/enable` | 按稳定 ID 管理任务；TC1 / S30 的功率规则见设备指南 |
| `buttons.get/set` | 分页按键映射，get 最多 8 条 / 页；按 next/capacity 继续 |
| `system.reboot/reset`、`ota.start` | 重启、重置、URL 升级；重置范围按设备能力与指南确认 |
| `night.get/set`、`time.get/set` | 夜间模式与时区，按机型提供 |
| `binding.get/set`、`binding.status.get` | A1 数据源设置及会话状态，M1 不提供 binding.set |
| `sensors.average.get` | A1 各有效字段的平均值，无有效样本为 null |
| `filter.get/set/reset` | A1 运行时间估算；寿命 1–20000 小时，reset 保留寿命并清零用量 |
| `display_profile.get/set`、`display_value.set` | RC5 M1 schema4 显示配置与原始显示写入；检查 MCU 能力 |
| `diagnostics.get` | RC5 按需诊断；字段编号和业务状态分开 |

A1 最多绑定 4 台数据源，检测仪最多保留 4 台 A1 的 RAM 会话。任务时间按配置时区，时钟无效时不执行周循环 / 一次性任务；错过不补执行。单调时钟倒计时、功率条件按各自能力执行。

M1 MQTT 来源绑定设备 ID、字段 ID、预期类型和单位；字段描述与实时快照使用 schema3，显示配置 GET 使用 schema4，SET 不带 schema 或 runtime。revision 冲突时重读配置，由用户确认后再保存，不覆盖其他客户端修改。

## 操作反馈

开关类控件先显示目标值及确认中，再下发一次设置命令。收到 `accepted` 后查询实际状态，达到目标再结束确认；设备拒绝或窗口内未确认时恢复最近读值并提示。旧轮询不得覆盖正在确认的目标，不自动重发开关命令。

同设备、同参数的在途只读请求合并，状态轮询不叠加。一次失败保留上次状态；连续三次失败且距最后成功至少 30 秒后提示暂时无法更新，恢复成功清除提示。传感器字段仍按设备有效性和年龄失效，不能沿用旧读数冒充新鲜数据。

首页趋势为浏览器本次打开期间最多 60 个采样点，不是设备持久历史。TC1 / S30 功率、电量单位使用返回值定义；`diagnostics.get`、内存和日志页面按需读取，避免首页反复拉取完整诊断。

## 版本与校验入口

RC5 实现入口：`common/service_mp.inc`、`common/service_product_mp.inc`、`common/httpd/`、`common/mqtt/`、`common/sensor_mp.h` 与 `common/lan_wire*.inc`。测试入口：`tests/test_service_mp.py`、`tests/test_http_product_mp.py`、`tests/test_native_mqtt.py`、`tests/test_sensor_mp.py`、`tests/test_native_udp_runtime.py`。公共前端锁定版本与 S30 适配另见各自来源记录。

稳定版 0.9.27 的 JSON 结构从对应标签读取，不对 RC5 请求做自动编码回退。升级和型号 / 变体选择见[固件更新](public/firmware-update.md)。
