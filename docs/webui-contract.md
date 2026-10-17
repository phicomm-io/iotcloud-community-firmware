# WebUI 设计与接口约定

TC1、A1、M1 的离线页面统一由 [前端仓库](https://github.com/phicomm-io/phicomm-community-frontend) 构建。固件提交 `frontend.lock.json` 锁定完整 commit 的 Release 与 SHA-256，`tools/frontend_release.py` 下载校验全部文件，`tools/pack_frontend.py` 直接嵌入其中的确定性 gzip，不再从固件仓库展开前端源码。浏览器无需 CDN。公共样式、词表、服务层、设置页及产品 `page.js` 均在前端仓库维护。`common/web/page.py` 仅为已有工具保留命令兼容入口，输出锁定 Release 中的 HTML。

## 页面与词表

- 标题显示设备类型与设备名称；一级导航为“首页”“设置”。首页展示设备的核心控制及测量值。
- 设置采用相同导航与卡片/表单：Wi-Fi、MQTT、设备、固件升级、诊断。设备专用功能采用相同组件和操作反馈。
- 固定使用“设备名称”“网络名称”“Broker 地址”“主题模式”“用户名”“密码”“访问认证”“网页密码”“童锁”“定时任务”“夜间模式”“按键事件”。网页认证用户名固定为 `admin`；MQTT 用户名独立可编辑。
- 状态值由公共词表翻译，分组诊断与原始 JSON 同时提供；用户输入通过 `textContent` 呈现。
- “请求已接受”表示接口接受，不等同于机械输出、物理按键或计量校准已验证。

## 公共 HTTP 定义

普通查询和控制统一使用 `POST /api/v1/command`，`Content-Type: application/json`。请求为 `{request_id?, action, params}`，响应为 `{request_id?, code, message, data}`；成功码为 `ok` 或 `accepted`。输入须完整接收并通过类型、长度和参数校验后才生效。写命令使用唯一 request_id，重试保持相同请求；已用 ID 对应不同内容返回 conflict。具体可用动作由 `capabilities.get.actions` 提供。

| 动作 | 用途 |
| --- | --- |
| `status.get`, `config.get`, `capabilities.get` | 状态、脱敏设置、能力 |
| `config.set` | `{section,values}`；device/auth/mqtt/wifi/ntp 及产品专用设置 |
| `wifi.scan`, `wifi.scan.get` | 启动扫描、读取结果 |
| `logs.get` | `{after,limit}`；分页读取公共 RAM 日志，最多 8 条/页 |
| `schedules.get/put/delete/enable` | 稳定 ID 的任务查询、保存、删除、启停 |
| `buttons.get/set` | 自定义按键映射；get 使用 offset/limit（最多 8 条），next/capacity/count；是否可写见能力列表 |
| `system.reboot/reset`, `ota.start` | 重启、重置、签名 OTA URL |
| `night.get/set`, `time.get/set` | 夜间模式（A1/M1）与时区 |
| `binding.get/set`, `binding.status.get` | A1/M1 绑定设置、运行状态 |
| `sensors.average.get` | A1 多 M1 实时平均值 |
| `light_effect.next/select` | A1 轮换灯效或选择 `{effect:0..4}`，保留其他灯效配置 |
| `light_always.set` | A1 灯环常亮 `{enabled:boolean}` |
| `filter.get/set/reset` | A1 运行时间估算；set 接受额定寿命 `{lifetime_hours:1..20000}`；reset 清零用量 |

文件 OTA 独立于 JSON 命令：`POST /api/v1/ota/upload` 上传签名原始字节流；仅显式选择未签名更新时使用 `/api/v1/ota/upload-unsigned`。两者仍校验产品镜像和写入结果。

密码不通过查询返回。MQTT 用户名最多 63 UTF-8 字节，密码最多 256 字节，Broker 最多 127 字节。网页省略空白密码输入以保留旧值，显式清除则传空字符串。Wi-Fi 开放网络同样须显式确认。网页认证用户名固定 admin。

## 能力区别

| 能力 | TC1 | A1 | M1 |
| --- | --- | --- | --- |
| 核心功能 | 六路插座、总开关、功率、电量 | 风机、RGB 灯环 | 空气参数、屏幕亮度 |
| 统一命令、认证、重启、OTA、日志诊断 | 支持 | 支持 | 支持 |
| 童锁 | 面板按键 | 电源/调速，配网键保留 | 不提供 |
| 完整定时任务 | 128 项 | 8 项 | 8 项 |
| 夜间模式 | 不提供 | 支持 | 支持 |
| 自定义按键事件 | 默认无映射，点击 1–30 / 长按 1–4 秒 | 按键能力见 capabilities | 不提供自定义写入 |

任务支持周循环和一次性触发。时间按配置时区；失去时钟有效性时不执行；错过的触发不补执行。动作依据各产品能力校验，重启类动作最后执行。公共日志为 64 条 RAM 环，重启不保留，不包含密码。

A1 `capabilities.get.button_keys` 为 `power/wifi/combination`，对应 `buttons.set.key` 的 `0/1/2`。每键支持点击 1–30 次或长按 1–4 秒，共 102 个槽位；分页按返回的 `next/capacity` 继续。示例：`{"action":"buttons.set","params":{"key":2,"clicks":1,"action":"light_effect.next","params":{}}}`。`filter.reset` 同样使用空动作参数。A1 默认电源单击使用 `fan.panel`（空参数）：关机时开机，开机时仅提示当前模式与实际风量；电源长按 1–4 秒默认 `fan.set` / `{on:false}`，松手执行。`fan.panel` 仅用于按键映射，不用于定时任务，受童锁与升级忙状态限制。A1 开关动作可用 `{toggle:true}`，`light.set` 可省略 brightness 保留亮度。灯效轮换、指定灯效及滤芯清零只用于按键事件。童锁阻止普通按键动作，但允许配置的童锁动作解锁；Wi-Fi 单键的 5/10 秒恢复手势独立保留。

## TC1 资源与兼容边界

- 固定日志环代替逐条 `malloc/free`，保留条数由 100 调整为 64；固定快照减少诊断读取时的分配。
- 遥测使用一次有界响应分配，按名称转义的最坏长度计算容量，避免 per-field JSON 对象堆；Wi-Fi 扫描最多保存 24 个网络，20 秒后退出扫描状态。
- 定时表在启动时从持久化值重建链表，忽略保存的指针；增删/替换/执行均使用锁，空表和容量上限有回归测试。
- 功率历史格式化有长度边界；长时间空载后的 IRQ 补记录循环最多处理环容量，计量系数与算法保持现有定义。
- 网络校时采用单调时钟安排重试，成功后 1 小时，失败后 60 秒，避免整点重复与快速重试。
- HTTP 与 MQTT 线程栈未凭推测缩小；没有物理按键、负载计量及长期耐久测试证据时，不宣称这些硬件边界已通过。

网页重启与旧 `soft` 请求均先发送响应，再通过 SDK 重启；软件重启不再把打包镜像头当向量表直接跳转。

## 当前统一接口补充

新前端所有普通查询/设置通过 `POST /api/v1/command`，动作名见 `capabilities.get`；旧产品 HTTP 路径仅作兼容，不是新前端接口。HTTP v1 与 MQTT 的 TMCP(v2)/Legacy(v1) 独立编号。MQTT 主题、公告、版本字段和平均值结构以 [MQTT 协议约定](mqtt-account.md) 为准。

A1 首页展示已绑定且新鲜的 M1 四项实时算术平均值；`sensors.average.get` 和 `binding.status.get.average` 同构。无有效采样显示破折号，不显示零值冒充数据。

A1 的 `status.get.filter` 与 `filter.get` 同构：`{used_seconds,lifetime_hours,remaining_percent,replacement_due}`。额定寿命默认 4320 小时；`filter.set` 仅接受 `{lifetime_hours:整数}`，范围 1–20000，不清除计时。`filter.reset` 仅接受空参数，清零计时、保留额定寿命；成功落盘后返回 `accepted` 和更新后的滤芯状态，落盘失败返回 `storage_error` 并回滚。OTA、重启和 Wi-Fi 试连接期间返回 `busy`。这些动作只作用于当前 A1，不同步至群组。HTTP 和 MQTT 使用相同命令入口及 request_id 去重规则。

计时依据风机已经施加的非零输出，精确累计秒数，不依赖网络校时或浏览器在线。百分比向上取整，耗尽后保持 0%；不超过 10% 时 `replacement_due=true`。每累计运行一小时、风机停止、正常重启/升级预留前保存；突发断电可能丢失最近不足一小时，闪存写入失败时的未保存区间可能更长（可从 `save_error` 诊断）。首次升级从零开始，网络重置保留，出厂恢复清零并恢复额定寿命默认值。旧固件无 `filter` 字段时前端隐藏卡片。

日志查询使用 `logs.get`，列表字段为 `records`，游标为 `next/latest`。TC1 单插座控制仅发一个指定 index 的 `outlet.set`，全开/全关不带 index。功率接口返回 `power_watts` 和 `energy_kwh`；首页趋势是本浏览器本次打开期间最多 60 个采样点，不是持久历史。单个采样点的 SVG 坐标也必须有效。

SDK 基础 Wi-Fi 扫描未提供安全类型时，网页不会推断为空密码网络；由用户显式选择开放网络，避免清除已保存的密码。

## 控制反馈与通信容错

开关类控件先展示目标状态并标记确认中，再下发一次设置命令。`accepted` 只代表请求已接收，必须重新查询设备状态，实读值与目标值一致后才能结束确认。确认期间，旧轮询不得覆盖目标状态；设备明确拒绝或在确认窗口内未达到目标时，恢复最近实读状态并提示未确认。不得自动重发开关命令。

同一设备相同参数的在途查询由管理平台合并；状态轮询不并发叠加。一次查询失败保留上次状态，不立即判定设备离线。首页仅在连续三次失败且距最后成功至少 30 秒后提示暂时无法更新，恢复成功即清除提示。传感器是否有效仍遵循设备报告，不用放宽通信提示代替传感器有效性判断。

回归：`node --test tests/test_web_control.cjs` 覆盖即时反馈、旧状态覆盖、拒绝回滚、未生效回滚和通信恢复。

## A1 / M1 看门狗诊断

`status.get` 新增 `reset_reason`（`watchdog` / `lockup` / `software` / `power_on` / `unknown`）、`reset_reason_raw`（SDK 原始位掩码）、`watchdog_enabled`、`watchdog_state`（`starting` / `healthy` / `fault`）、`watchdog_fault`（空字符串或故障槽名）。复位原因启动时读取一次，本次运行保持不变。

诊断页按上述字段显示“看门狗与复位原因”。上次是 watchdog 复位时显示“上次因看门狗超时重启”；当前故障槽仅在等待硬件复位的短暂窗口内可读，不写 Flash。启动日志事件为 `{module:"system",event:"watchdog"}`，日志页补充对应说明。启动时重建日志，下一次上电会重新读取原因；此功能不提供持久故障历史。TC1 不发布上述字段。
