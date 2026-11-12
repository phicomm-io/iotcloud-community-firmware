# BRY88BBA15G screen services

Community services reuse iotcloud/openiot. Commands use the existing authenticated
`POST /api/v1/command`, `Content-Type: application/msgpack`, with the envelope
`{request_id, action, params}`. Shared MQTT/LAN use the same business dispatcher.
Factory UART JSON/topics remain an internal compatibility boundary.

## Remote paging and display trace / 远程换页与显示诊断

WebUI: **Screen → Display → Page control** and **Screen → Diagnostics**.
These actions use the existing MessagePack command envelope over HTTP/MQTT/LAN;
parameter objects below are notation, not a new JSON endpoint.

- `screen.page.get {}` returns supported/available, request/state and the MCU's
  one-based `current`, `enabled_mask`, `count`, `asleep`, `layout_revision`, `age_ms`.
- `screen.page.set {direction:"next"}` or `{direction:"previous"}`, or
  `{page:3}` returns an accepted request ID. Poll `.get` for matching `complete`;
  `invalid_page`, `mcu_busy` and `timeout` are failures. Disabled pages are skipped
  and disabled go-to targets rejected. Page changes wake the display and never
  save a new layout or sleep policy. The capability is 0x100000.
- `screen.diagnostics.start {mode:"current"|"next"|"page",page?:3}` starts a
  capture. `page` is only valid with mode `page`; the screen must be awake.
- `screen.diagnostics.read {offset?:0,limit?:8}` requests a summary/trace batch;
  `screen.diagnostics.get {}` returns `pending`, `timeout`, `available`,
  `reply_request`, generation, capture state, measured scan period, flush and
  monitor totals, and records. Match the returned request ID, then use the same
  generation while fetching subsequent eight-record batches. The MCU retains
  96 records and counts overflow. `screen.diagnostics.stop {}` disables capture.
  The capability is 0x80000. The scan source is LCD frame-end IRQ, not a probe.

网页“显示”提供上一页、下一页和指定页，等待 MCU 确认，自动跳过禁用页并唤醒屏幕。
“诊断”采集刷新区域、flush 微秒耗时、绘制间隔及 LVGL 刷新轮次，可本地导出。
旧 MCU 不声明能力时禁用相关操作。页面状态每秒及换页时回报，超过五秒视为失效。

## Weather memory and retries / 天气内存与重试

Weather admission uses byte-addressable internal heap, not total ESP heap that
also includes 32-bit-only IRAM. The initial budget is 72 KiB for Open-Meteo,
84 KiB for other providers, with a contiguous block of at least 24 KiB. Allocation
or request failures back off 30/60/120/240/300 seconds. Manual refresh may bypass
the scheduled retry after a 15-second minimum spacing and cannot overlap a worker.
TLS handshake and HTTP timeouts are six seconds. Certificate verification follows
the configured setting, default false. A failed fetch keeps the previous cache.

The G3 status encoder releases its 6 KiB JSON scratch pool after publishing the
owned MessagePack snapshot. Completed delayed UART messages release their String
buffers as well. These allocations are not retained between weather requests.

The 9,520-byte SCN3 wire cache uses unused 32-bit-only IRAM first, with aligned
word accesses and byte-addressable chunks for UART/HTTP. A DRAM fallback preserves
layout support if that allocation is unavailable. Encoding uses temporary byte
scratch once per revision, then releases it; subsequent 512-byte sync packets
reuse the cached bytes and CRC. Allocation failure leaves the previous cache
intact and delays automatic retry by two seconds. `screen.status` reports
`layout_cache_bytes` and `layout_cache_storage` (`iram`, `dram`, `unallocated`).
Weather requests wait for active layout synchronization to finish.

An idle layout upload no longer reserves about 9.3 KiB of ESP RAM; storage is
allocated only for an active transaction and freed on commit, invalidation or
30 seconds without an accepted chunk. The weather configuration pool is 1536
bytes, checked for overflow before persistence. TLS and layout upload admission
exclude one another. `screen.weather.get` exposes byte heap and largest block,
worker start/end values, attempts, consecutive failures and retry delay. Diagnostics
fields 101–103 expose free/largest/minimum byte heap for this product.

天气请求按可逐字节访问的内部堆判断是否启动；内存不足与失败都有退避，避免循环申请。
手动刷新至少间隔 15 秒，握手与 HTTP 均有超时。默认关闭证书校验，失败保留旧天气。
G3 状态编码的 6 KiB JSON 临时区在发布快照后释放，已发送的延迟串口消息也释放底层
字符串缓冲，避免这些空闲占用阻止后续天气刷新。
9,520 字节的布局编码缓存优先使用只能按 32 位访问的空闲 IRAM，通过对齐的整字读写
与通信字节缓冲交换；申请失败可回退到 DRAM。每个布局版本只编码一次，临时区随后
释放，后续串口分包复用缓存及 CRC。申请失败保留旧缓存，自动重试间隔为两秒。
状态提供缓存大小及存储类型，天气请求等待正在进行的布局同步完成。
布局暂存区按需申请并限时释放；天气配置缩小后仍校验完整性再保存。运行开始/结束的
堆统计用于定位分配失败，不等同于请求期间的最低内存峰值。

## Screen brightness / 屏幕亮度

In **Settings → Device → Screen brightness**, load settings and adjust 0–100%.
The editor applies changes live with a 350 ms debounce, coalescing the latest
value while an acknowledgement is pending; explicit save remains available.
The editor reads the display group again before saving so
`data_type`, `power_screen_time` and `battery_screen_time` retain their current
values. It waits for the matching MCU acknowledgement, a fresh settings snapshot
and, when supported, the MCU's brightness report. Rejection, timeout and another
request replacing its token are shown as unconfirmed. Background status updates
preserve an unsaved draft; the slider stays usable during a pending save.

- `screen.settings.get {}` returns `settings` from the MCU, `saved_settings`
  on the ESP, `snapshot_age_ms`, `request:{token,code,state}` and `brightness`.
- `screen.settings.set {display:{luminance:5,data_type:0,power_screen_time:2,
  battery_screen_time:1}}` saves the group durably before queueing the original
  factory UART command. It returns `accepted` and a token; the MCU result and
  readback are separate. Existing validation and busy/OTA gates still apply.
- Request states: `idle`, `pending`, `confirmed`, `rejected`, `timeout`.
- `brightness` also appears in `status.get.screen`. It includes `supported`,
  `available`, `requested_percent`, `ceiling_percent`, `applied_percent`,
  `asleep`, `age_ms`. V2 adds `target_percent`, `sleep_reason`, `page` and
  `policy_revision`. It requires negotiated capability 0x10. The actual PWM
  register is read by the MCU; it is never inferred from the requested setting.
  Reports expire after 15 seconds and are invalidated by session/recovery state;
  unavailable values are null. Original MCUs do not provide this telemetry.

ESP persistence restores the display group when the community MCU reconnects,
including after ESP restart. A 0% request keeps networking and sensors running
with the backlight off. Negotiated sleep can keep a dimmed composed page; first
touch wakes without paging. The default runtime ceiling is **100%**, configurable
through display policy. Before policy negotiation the fallback is also 40%;
startup output is at most 10%. The editor shows requested and actual values
separately. Percentages are PWM duty, not nits.

Configuration saves size the JSON document for the live weather/settings and
reminder records. Empty reminder arrays no longer require a contiguous 40 KiB
allocation. Pool allocation, complete layout encoding and full document
construction must succeed before the existing atomic storage commit runs.

网页入口为“设置 → 设备 → 屏幕亮度”。加载后调节即生效，待应答时合并最新操作，
也可手动保存。保存前重新读取
显示设置，保留原有显示类型和两项熄屏时间。保存后等待 MCU 同一请求的应答、
新快照与亮度回报，拒绝、超时或被其他请求覆盖时不显示为已确认。设置由 ESP
持久化，在社区 MCU 重连及 ESP 重启后恢复。0% 仅关闭背光，网络和传感器继续
运行；休眠可保留低亮度组合页，首次触摸只唤醒。默认 **100% 运行上限**，启动
业务初始最多 10%，运行上限通过策略配置；网页分别显示
设定值和实际输出；原厂 MCU 无实际亮度回报时明确显示未知。

配置保存按实际天气、设置与提醒数量计算缓冲区，不再为没有提醒的配置固定
申请 40 KiB 连续空间；分配、布局编码或文档构造失败时保留原配置。

## Clock / 时间同步

SNTP starts after Wi-Fi has an IPv4 address, and restarts after reconnect or an
address change. The SDK handles DNS, retries and server replies asynchronously;
UART and OTA processing keep running while the device waits for time. The
existing factory `+sntp` response and negotiated `SC_MSG_CLOCK` remain unchanged.

`status.get` includes `screen.clock`: `valid`, UTC seconds (`utc`),
`timezone_minutes`, `state` (`waiting_network`, `waiting_ntp`, `synchronized`),
`sntp_synced` and `sync_age_ms`. `sntp_synced` records an actual SDK completion
since this ESP boot; a retained clock alone does not set it. The MCU's last
snapshot provides `mcu_snapshot_utc` and `mcu_snapshot_age_ms`. When that sample
is less than 60 seconds old, `mcu_offset_seconds` compares its elapsed time with
the ESP clock; allow for UART delivery delay. Invalid or unavailable values are
null. Display and reminder timezone use the same saved minute offset.

SNTP 在 Wi-Fi 取得 IPv4 地址后启动，重连或地址变化后重新发起，避免在网络尚未
就绪时进入请求退避。DNS、重试和应答由 SDK 异步处理，等待期间串口与 OTA 继续
运行。原厂 `+sntp` 和协商后的 `SC_MSG_CLOCK` 格式保持不变。

`screen.clock` 返回 ESP 的 UTC 秒、分钟时区、同步状态和最近同步经过时间。
`sntp_synced` 只在本次 ESP 启动后收到 SDK 同步完成通知时置位；仅有保留时间
不会视为 NTP 已同步。MCU 快照时间及其年龄用于比较两端走时，样本超过 60 秒
不再给出偏差；比较允许串口投递延迟。无效时间返回 null，显示与提醒共用已保存
时区。RTC 无有效日历且 NTP 尚未成功时，屏幕继续显示 `--:--`。

## Display policy / 休眠与每周计划

`screen.display.get {}` returns `supported`, saved `revision`, MCU
`applied_revision`, `error`, `max_plans` and the policy. `screen.display.set`
requires all six fields and community capability 0x20:

```text
{ceiling:100, external_minutes:2, battery_minutes:1,
 idle_brightness:5, idle_page:0,
 plans:[{name:"Night",enabled:true,days:127,start:1380,end:420,brightness:5,page:0}]}
```

Minutes are 0..1440 (zero disables idle sleep); ceiling is 1..100 and brightness
0..100. Page is an enabled zero-based layout page or 255 to retain the current
page. At most eight plans, with Monday at day bit 0; each start/end is a local
minute 0..1439 and must differ. Overnight plans belong to the starting day;
first matching plan wins, with start included and end excluded. Touch wakes
without paging and overrides this occurrence. An invalid clock disables weekly
matching, not monotonic idle sleep. OTA has priority. ESP persistence precedes
acceptance; the MCU revision ACK is separate and re-sent after reconnect.
Stock MCUs return `unsupported` and continue using factory timeout settings.

网页支持外部供电／电池休眠时间、休眠亮度、组合休眠页和八项每周计划。跨午夜
按开始日匹配，按列表优先级执行；触摸覆盖本次时段，唤醒恢复休眠前页面。
策略保存和 MCU 应答分别显示；旧 MCU 继续使用原厂超时逻辑。

## Layout and external data / 页面与数据

The web editor previews the full 480×854 portrait canvas at a fixed aspect
ratio. It follows the MCU geometry: 16 px outer margins, 8 px gutters and a theme-colored page strip. The canvas and labels scale together.
Composed pages support drag/resize, keyboard adjustment, 20 layout undo steps
and device-specific browser drafts. A matching base revision restores a draft
automatically; stale drafts require explicit loading.

网页布局预览按完整的 480×854 竖屏等比缩放，保留顶部标题和分段页码。
组件使用 16 像素外边距和 8 像素间隔；手机和
桌面使用相同比例。组合页支持拖动、缩放、撤销与浏览器草稿，保存到设备需另行提交。

- `screen.layout.get {offset:0, limit:512}` pages through SCN3 (9520 bytes), returning
  base64 `bytes`, `offset`, `total`, `revision` and whole-image CRC-32.
- `screen.layout.begin {revision, total:9520, crc}` then contiguous
  `screen.layout.chunk {offset, bytes}` and `screen.layout.commit {revision}`.
  Revision must increase. UTF-8, type, IDs, grid bounds, overlap and CRC are
  checked before durable replacement. `accepted` means saved on ESP32;
  `screen.mcu_layout_revision` reports the actual MCU ACK.
- `screen.data.push {id, values:[...], text:"...", ttl:60}` requires an
  external-bound component ID, at most 64 finite values, text ≤192 UTF-8 bytes,
  TTL 1..86400 seconds. Data is volatile. A stock MCU returns `not_ready`.
- `screen.data.series {id,ttl:60,series:[{name:"Room",unit:"°C",
  values:[21,null,22]}]}` requires capability 0x40 and an external-bound chart.
  Up to three series ×32 points, finite absolute values ≤1e9, TTL 1..86400;
  null creates a gap. Additive charts require the same unit and nonnegative
  values. Reads do not persist this volatile chart data.

SCN3 is little endian: 16-byte header (`SCN3`, u32 revision at 4, page count at 8,
item count at 9), eight 36-byte page records and 32 items of 288 bytes.
Pages have stable 24-bit IDs. Types 0..7 are clock, value, text, line, bar, pie,
weather and reminders. Bindings 0..5 are temperature, humidity, CO₂, PM2.5,
TVOC and HCHO; 6 is external. Grid 4×6, at most eight items per page.
Items support static text, three bindings, history ranges and seven chart modes:
line, multi-line, dual-axis, bar, stacked bar, stacked area and ring.
Normal widgets require 2×2 cells; full-width values support 4×1 and charts 4×2.
Only the locked RC6 community ABI is supported; older community layouts and
telemetry versions are rejected. The factory MCU continues to use stock messages.
Deleted-page policy references are remapped by stable IDs or reset to keep-page.

SCN3 item byte 82 stores `value_mode`: 0 automatic, 1 number, 2 grade. Nonzero
modes require a value widget and negotiated capability 0x80. TVOC number mode
uses ppb; grade mode keeps the factory hysteresis. External grade mode displays
fresh `screen.data.push` text. Other local measurements remain numeric. An
unsupported mode is rejected before replacing the saved layout.

SCN3 组件第 82 字节保存显示方式：0 默认、1 数值、2 等级。TVOC 支持 ppb 或
原厂等级；外部等级取有效 API 文字，随布局持久化。非默认方式需要能力 0x80，
旧屏幕不支持时编辑器禁用选项，提交也会明确拒绝。

能力 `0x100` 增加 4×1 数值和 4×2／4×3 紧凑图表。方形数值按尺寸放大，
标题与单位同排；整行数值左侧标题／单位、右侧大数字。紧凑图表左一列显示
标题和读数，右三列绘图。新布局在保存和下发时均检查能力，不改变 SCN3 编码。

## Optional NOR resources / 可选 NOR 资源

`screen.resources.get {}` and `status.get.screen.resources` report
`available,state,external_font,fallback,glyphs,bytes,crc`. They require capability
0x40; telemetry expires after 15 seconds or a session/recovery change. State is
`unchecked`, `absent`, `ready`, `io_error`, `invalid`, `timeout`, `uploading`,
`preparing`, `loading` or `unknown`.

Negotiated capability `0x40000` enables a 24-byte resource report (`0x2a`,
version 2): state at 2, fallback at 3, glyph count/LE16 at 4, read mode at 6,
zero at 7, font bytes/LE32 at 8, payload CRC/LE32 at 12, elapsed milliseconds
at 16 and received bytes at 20. Read modes are 0 untested, 1 GPIO, 2 SFC
single and 3 SFC quad. The API adds `read_mode,load_ms,received` only for a
fresh version 2 report. Version 1 reports are rejected.

SFC uses read-only hardware transactions at system clock / 8, with GPIO data
comparison on first use and GPIO fallback after controller failure. Font reads
update CRC incrementally in slices of at most 8 KiB or 8 ms. Drawing uses the
validated SDRAM copy and does not read NOR.

The Resources tab supports `screen.font.get/begin/chunk/commit/cancel` with
capability `0x4000`. Begin supplies the base64 32-byte IOTFNT02 header; chunks
supply `offset` and base64 `bytes` (1–512 bytes), waiting for the matching
request/session ACK before continuing. Commit validates the complete file and
writes the bank descriptor last. Wallpaper, font and MCU writers are exclusive.
Status includes request, state, code, resource_state, preparing, active, received,
total, crc and generation. Preparation is bounded; a query does not retry writes.

The two 2 MiB banks at 0xb00000 and 0xd00000 are claimed only after a complete
blank scan or a valid owner tag. Unknown nonblank content is preserved. Data is
read back per chunk, then cached and validated in SDRAM. The newest invalid bank
falls back to the other complete bank once, then built-in fonts. The legacy
0x900000 bank remains read-only. Original assets and OTA areas are preserved.
Resource errors never request an automatic reset. OTA storage failure cancels
without activation. See the screen repository `docs/EXTERNAL-FONT.md`.

资源标签可上传高清字库，每块最多 512 字节并逐块应答、回读；整个文件验证通过后
才提交。双资源区先检查空白或所有权，未知数据不擦除。失败回退内置字体，绘制
只访问 SDRAM；通信、传感器与 UART OTA 继续运行。

## Headings, points and language / 标题、点数与语言

SCN3 item byte 83 stores hide_title (0/1); bytes 84–85 store local chart points
(0 keeps 32; otherwise 32, 64, 128, 256). Required capabilities are 0x2000 and
0x10000. Hiding a title also reclaims its heading space. Local history still
samples once per minute and stretches short history. External series retain
32 points each. APP1 byte 56 stores screen language (0 Chinese, 1 English),
requiring 0x8000. Its size remains 704 bytes. Browser language is independent.
Old zeroed fields retain old behavior; unsupported extensions are rejected.

APP1 byte 57 stores the page rail height: 0 keeps the default 12 px, or use
6, 12, 20. Nonzero settings require capability `0x20000`; bytes 58–63 remain
zero. Old APP1 documents retain their encoding. The web editor uses the same
height as LVGL, moving the title below thicker rails and preserving SCN3 bytes.

组件可隐藏标题，并为内容腾出空间；本地曲线可选择 32、64、128、256 点。
显示标签中的屏幕语言独立保存，支持内置标签和天气的中英文翻译。自定义文字不
自动改写。三项扩展经能力声明使用，保留原配置长度和原厂串口回退。


## Reminders

- `screen.reminders.get {offset:0}`: eight slots per call, offsets 0,8,16,24;
  returns rules, notices, timezone and default wake policy.
- `screen.reminders.configure {timezone:480, wake:0}`: fixed UTC offset in
  minutes (-720..840); wake 0 obeys DND, 1 always, 2 only for unread notices.
- `screen.reminders.set {id:1, kind:2, epoch:0, minute:540, days:31, wake:255,
  enabled:true, title:"Ventilate", text:"Open the window"}`: rule IDs
  1..0x7fffffff; kind 0 once at UTC epoch, 1 daily, 2 weekly. Minute is local
  minute of day; Monday is weekday bit 0. Wake 255 inherits the global policy.
  Title ≤64 and text ≤127 UTF-8 bytes. Changes persist before success.
- `screen.reminders.delete {id:1}` removes a rule.
- `screen.reminders.push {title, text, wake:0}` creates a durable unread notice,
  returning its ID; an optional caller ID must be ≥0x80000000.
- `screen.reminders.ack {id}` durably marks a notice read.

Maximum 32 rules and 32 notices. Requires synchronized time; boot and large time
jumps do not replay a backlog. Fixed timezones do not implement DST transitions.

## Sensor calibration / 传感器校准

Requires matching community ESP32 and MCU firmware. Supported sensors are CO₂
(factory fixed 400 ppm reference) and TVOC (factory command). Temperature and
humidity retain factory compensation; HCHO installation only acknowledges, and
no manual PM2.5 calibration service was found.

- `screen.calibration.get {}` returns `supported`, `available`, `pending`,
  `co2`/`tvoc` (`ready`, `warmup_seconds`, `samples`, `state`) and `request`.
- `screen.calibration.start {channel:"co2"|"tvoc",environment_confirmed:true}`
  queues one existing factory service request after checking freshness, warm-up
  and pending work. CO₂ needs 3 minutes, TVOC 5 minutes 20 seconds of continuous
  valid operation. Sampling takes about 2 minutes; an unstable range ≥201
  rejects the command. The MCU repeats readiness checks against actual samples.
- `screen.calibration.cancel {}` cancels sampling before a command has been sent.
- An accepted API response is a queue receipt. `request.state=accepted` is the
  MCU service acknowledgement. Sensor state `command_sent` means the UART
  accepted transmission; CO₂ `acknowledged` requires its checksum-valid reply.
  Other terminal states: `failed`, `sensor_lost`, `cancelled`. TVOC completion
  acknowledgement has not been verified. No automatic calibration retry occurs.

CO₂ requires an independently confirmed 400 ppm environment, stable for at least
2 minutes before starting and throughout sampling. TVOC uses clean, ventilated
air without volatile contamination and stable readings; its complete command
conditions were not found in public manufacturer documentation. Source analysis
and primary references are in the MCU repository's `docs/SENSORS-CALIBRATION.md`.

## AM1008L / PD12

`screen.module.get {}` returns the negotiated module-control status. With MCU
capability bit 10, `screen.module.set {set_high:false,duration_ms:10000}` leases
PD12 low; duration is 1..60000 ms. `set_high:true` restores high immediately
and requires zero duration (or omission). A request first returns `accepted`;
poll `state`, matching `request` and GPIO `set_high` readback for completion.
`remaining_ms`, `valid_frames` and `transitions` report the lease and UART2
activity. Stale readback is null, not a successful state. Low is never persisted,
and manual lease expiry or link reset restores high during normal business.
Startup and MCU OTA have an independent low interlock until business/transfer
completion. ESP uploads wait for low readback and renew a 60-second lease every
15 seconds, releasing it on failure or completion; successful ESP app OTA holds
through reboot. A live minimal recovery service owns its own low interlock;
uploads bypass the unavailable business-level module ACK in window, held and
fault recovery modes. Unsupported peers retain the original upload path and
expose no controls. The WebUI provides both actions and a bounded duration field.

`screen.module.get` 查询协商后的状态，`screen.module.set` 软件控制 PD12。拉低须给出
1..60000 毫秒，超时自动恢复；恢复高电平可随时执行。接口先返回 accepted，再通过
请求编号、状态和 GPIO 回读确认。剩余租约、有效 UART2 帧数和电平变化次数可查询，
回读过期则显示未知。低电平不保存，普通业务中租约过期或链路重置恢复高电平。
开机维护窗口和 MCU OTA 由独立互锁保持低电平，业务／暂存完成或失败后恢复。
ESP 上传先确认低电平，并每 15 秒续租一次 60 秒租约；失败或完成释放，ESP 自身
更新成功则保持至重启。最小维护服务已自行保持低电平，启动窗口、保持和故障
维护期间上传不再等待普通业务的模块应答。未声明能力的原厂屏幕保留原升级通道。

## Weather

`screen.weather.get {}` returns redacted configuration and status, current
metrics and available `days` (date, text, low/high in °C). `last_sent_age_ms`
reports the last normalized UART enqueue for the current community MCU session,
or the last valid factory-weather UART write. It is not a screen rendering
acknowledgement. `delivery` is `community` or `factory`. On the factory path,
`factory` and `factory_sent_age_ms` expose the last written fields and their age:
whole-degree `temperature`, whole-percent `humidity`, `pm25:-100` (outdoor PM2.5
is not fetched) and `weatherCode` 1..8 (sun, partly cloudy, overcast, rain, snow,
fog, haze, wind/dust), or 0 when unavailable. Missing temperature/humidity use
the factory sentinels -100/200. Fresh fetches push updates; disabling weather
or reaching the cache TTL clears the stock values. Three-day forecasts and
location labels require the community MCU. `worker_stack_free_bytes` is the last
worker's minimum free stack measured by ESP-IDF, in bytes.
`request_stage` distinguishes DNS, TCP connection, TLS setup/handshake and HTTP
failures so a connection timeout can be diagnosed without disabling HTTPS.
`remote_ip` is the last resolved server IPv4 address. `connect_ms` includes DNS,
TCP and TLS setup for the last connection attempt; `connect_errno` samples errno
after a failed connection and is supporting diagnostic information, not proof
of which layer failed. These fields reset when a new fetch starts.
`screen.weather.set {enabled, provider, host, latitude, longitude, label,
interval, key?, clear_key?, verify_tls?}` saves configuration. Omit key to keep the saved
secret; `clear_key:true` removes it. Reads only expose `key_set`. Coordinates are
bounded, interval 300..86400 seconds (default 900).
`screen.weather.refresh {}` schedules a fetch when enabled.

| provider | API | Configuration |
| --- | --- | --- |
| openmeteo | /v1/forecast, current + three daily forecasts in one call | Fixed api.open-meteo.com, no key for personal/non-commercial use |
| qweather | /weather/v1/current, /weather/v1/daily | User host ending .qweatherapi.com; X-QW-Api-Key |
| seniverse | v3 current and daily | Fixed api.seniverse.com, API key |
| openweather | 2.5 weather and 5-day/3-hour forecast | Fixed api.openweathermap.org, API key, metric units |

`verify_tls` defaults to false: HTTPS skips server certificate verification.
Set it to true to enable verification. The WebUI exposes the same saved setting.
Verified mode uses the provider's CA roots. No arbitrary URLs or
redirects. Worker bounds response/decompression size, heap and timeout. Missing
fields stay missing; results are normalized before UART transmission.
TLS handshakes allow up to 30 seconds for Open-Meteo and 15 seconds for the other
providers on the 80 MHz G3. TCP connect and HTTP I/O timeouts remain 6 seconds.
Seniverse uses its DigiCert Global Root G2 PEM directly, avoiding an incompatible
cross-signed chain path in the ESP32 certificate bundle callback while retaining
hostname, expiry and signature verification. QWeather requests use `zh-hans` for
Chinese; Seniverse uses `zh-Hans` according to its separate API convention.
Open-Meteo uses Celsius, m/s, UTC Unix timestamps and the location's returned
UTC offset for daily dates. Unknown WMO codes, missing required values and
incompatible units are rejected. Its response and decode buffers are capped at
4 KiB each. The other providers require a user's working key and plan.
Gzip decoding allocates the ROM miniz decompressor on the heap: its 11 KiB state
exceeds the worker's 8 KiB stack. Truncation, output limits, CRC and size are
checked before JSON parsing. The output allocation follows the bounded gzip
size, and the compressed input is released before allocating the JSON document.
A failed layout base64 allocation aborts a config
save instead of committing an empty layout during TLS memory pressure.
Weather and diagnostics reads use bounded, smaller command documents while
TLS runs. A failed command-document allocation returns a complete `busy`
envelope before dispatching any mutation.

Open-Meteo 个人非商业使用免 Key，可在网页直接选择；其他服务商仍需填写自己的
密钥。以广东广州为例，纬度 `23.1291`、经度 `113.2644`，建议每 `900` 秒刷新。
地点可填 `Guangzhou, Guangdong`，当前已安装屏幕字库和天气描述均支持英文。
单次 HTTPS 同时获取当前天气与三天预报，单位为 °C、%、m/s，预报日期按地点
时区计算。免 Key 配置会删除之前保存的密钥和自定义 Host。网页提供服务来源署名。
`verify_tls` 默认关闭，保留 HTTPS、跳过服务器证书校验；网页可明确开启“校验服务器证书”，
选择随天气配置保存，`screen.weather.get` 返回当前有效设置。
Gzip 解压状态使用堆内存，避免 11 KiB 解压器占用 8 KiB 任务栈；解压后校验长度
和 CRC。保存配置时若布局编码分配失败，保留上一份配置。
天气和诊断查询缩小临时文档，给并发 HTTPS 留出内存；命令文档分配失败时，
返回完整的 `busy` 应答，且不执行配置写入。

Official references: [Open-Meteo forecast and WMO codes](https://open-meteo.com/en/docs),
[Open-Meteo terms](https://open-meteo.com/en/terms),
[QWeather current](https://dev.qweather.com/docs/api/weather/weather-current/),
[daily](https://dev.qweather.com/docs/api/weather/weather-daily-forecast/),
[authentication](https://dev.qweather.com/docs/configuration/authentication/),
[Seniverse current](https://docs.seniverse.com/api/weather/now.html),
[daily](https://docs.seniverse.com/api/weather/daily.html),
[OpenWeather current](https://openweathermap.org/current),
[forecast](https://openweathermap.org/forecast5).

## Storage and OTA

Screen uploads invalidate the artifact header first, then erase each payload
sector only when contiguous HTTP data reaches it. A partial upload cannot be
activated. Full payload hash readback precedes the final header commit. This
avoids an initial erase burst while leaving durable configuration untouched.

屏幕镜像上传先使暂存头失效，再随连续 HTTP 数据到达逐扇区擦除；完整回读摘要
通过后才提交暂存头。中断的镜像不能激活，独立持久配置区不参与擦除。

Original five partitions remain unchanged. Storage requires an 8 MiB physical
Flash and exact stock partition table, then probes the entire unallocated
0x700000..0x7fffff region before ownership. Unknown contents are not erased.
Configuration has two 64 KiB slots and a 48 KiB maximum payload; CRC, commit and
readback give old-or-new recovery after interrupted writes.
MessagePack is serialized into length-bounded bytes rather than an Arduino
String text writer. Boot loading allocates the actual validated record size;
`screen.config_load_error` exposes a failed load instead of hiding it.

The stock bootloader declares 4 MiB on this 8 MiB chip. After physical capacity
and table checks, initialization restores only the IDF RAM handle's size to the
detected capacity for explicit tail access. It does not alter the bootloader,
table, flash mode, ROM or cache mapping. Rejected tail probes restore the old
bound. See inspected fixed [IDF 4.4 initialization](https://github.com/espressif/esp-idf/blob/v4.4.7/components/spi_flash/esp_flash_spi_init.c)
and [physical-size implementation](https://github.com/espressif/esp-idf/blob/v4.4.7/components/spi_flash/esp_flash_api.c).

Authenticated `POST /api/v1/screen/ota/upload` accepts a raw 421888-byte MCU
application and requires `X-IotCloud-Target: BRY88BBA15G-screen`. It checks target
marker, vectors and size, stores it on ESP, and checks complete MD5/SHA-256
readback before committing metadata. Returns hashes and `installable:false`;
`screen.update.get` reports the runtime installation capability. Uploading alone
never starts a transfer or installation.

- `screen.update.get {}` reports artifact/transfer state. `ota.boot_backup_required`
  is false; `ota.gate` is `factory_uart` on the original MCU, `ready` on a current
  community MCU, or `staging_only_mcu` when activation is unavailable.
- `screen.update.start {mode:"stage"}` works only with a community MCU or its
  recovery mode, preserving the stock UART flow and stopping after NOR readback.
- `screen.update.cancel {}` stops staging.
- `screen.update.install {sha256:"..."}` explicitly activates the staged image.
  It requires v3 maintenance and the exact uploaded SHA-256. The MCU rechecks
  the application MD5 and erased source tail before writing/read-verifying A1
  and restarting. The paired MCU no longer checks a boot MD5 allowlist. Backup
  is optional; `flash_screen.py --boot-backup ... --reuse-boot-backup` can verify
  an existing pair without changing its original session metadata.
- Stock staging is blocked because stock firmware auto-installs. The explicit
  `screen.update.start {mode:"factory-bootstrap",sha256:"..."}` exception accepts
  a complete target application or the optional recovery image, with the exact
  uploaded SHA-256. Stock-version and recovery-only gates are removed. The
  command explicitly starts an installation, using the existing factory loader.

During the binary transfer, business UART messages are deferred. Weather,
clock, layout and reminder traffic cannot enter the raw packet stream; delayed
business queue entries are retained and resume after OTA.

引导区不随应用升级而写入。备份是可选操作；指定备份时仍检查双份内容一致，
`--reuse-boot-backup` 保留原会话记录。新 MCU 只复核应用摘要与复制尾区，
不再使用引导 MD5 白名单。已安装的旧 MCU 仍执行旧门控，替换后才生效。
二进制 OTA 期间暂停天气等扩展消息及延迟业务队列，
传输结束后再继续，避免业务文本进入按长度接收的原始数据包。

Successful installation requires a new MCU session and `screen_image 1` MD5
matching the actual internal application bytes. A completed transfer alone is
not success. The inspected example loader SHA-256 is
`ccb9d414186c5ea47823badf5083f7ab5b1b61b02c6a8d0510af6ee31bebdf09`.
This is a reference digest, not an allowlist. Its copy loop exceeds the application range; see the screen repository's
`docs/BOOTLOADER-20261105.md`. Unknown copy-tail contents are preserved and block
activation. MCU A/B rollback and power-loss installation recovery are not proven.

For ESP32-only development, existing `/api/v1/ota/upload-unsigned` checks the
exact BRY88BBA15G identity and writes only the inactive app slot and otadata.
Normal signed OTA remains the default upload route.

## MCU startup maintenance and boot backup

These authenticated MessagePack commands require the community MCU's independent
`screen_recovery 3` session; they do not require UI capability negotiation.
Explicit installation support is independent of UI startup.
The MCU starts with a 30-second SRAM-only window before display, SDRAM and sensors.
The ESP32 defaults to sending the existing resume operation after five seconds.
`screen.startup.set` persists `{"auto_resume":false}` to keep the full window;
`screen.startup.get` reports the saved setting, delay, session and request.
A manual hold or resume suppresses automatic action for that MCU session. Busy
OTA/resource transfers defer it. The recovery-only build stays in that service.
WDT/fault recovery never resumes automatically. This policy does not modify the
factory bootloader or bypass its power-on key gate.

| Action | Parameters and result |
| --- | --- |
| `screen.startup.get` | Saved `auto_resume`, `delay_seconds` (5), `window_seconds` (30), manual suppression and automatic request/session |
| `screen.startup.set` | Exactly one boolean `auto_resume`; transactional persistence, unchanged on storage failure |
| `screen.recovery.get` | Reports availability, session, mode, remaining window, actual internal image MD5, activation support, exported byte count and inherited WDT CR/RSTVAL/CLKSEL/RSTSR |
| `screen.recovery.hold` | Holds a normal startup window; asynchronous request/session |
| `screen.recovery.resume` | Releases that hold; not supported in forced recovery or after normal startup |
| `screen.recovery.nor.set` | Startup only: exactly one `mode`, `sfc_quad` (default), `sfc_single`, or `gpio`; selects the NOR bus for this boot and holds the window. Reset restores quad SFC; no automatic bus downgrade. |
| `screen.recovery.enter` | Requests software reset into maintenance; async reply can be lost as the session changes |
| `screen.boot.read` | `{offset,length}` within `[0,81920)`, length 1–256; async request/session |
| `screen.boot.result.get` | `{request}`; state/result/offset/total, plus length/Base64 bytes/CRC32 on a completed read |

Only one operation can be pending; MCU OTA and boot reads are mutually exclusive.
Frames validate nonce, sequence, CRC, exact response size and offsets. Stale MCU
announcements become unavailable after 15 seconds. `ota_activation` is true only
for a v3 service. Current MCU builds verify the reviewed loader locally and
accept a reusable matching backup.
The stock MCU returns `not_supported` for boot reads and is not reset by a query.

Diagnostic MCU builds add a `diagnostic` object to `screen.recovery.get` (also
nested in status). `available` requires matching-session telemetry received within
15 seconds; other fields are omitted when unavailable. Fields are `version`, `reason`,
`stage`, `previous_stage`, `ipsr`, `cfsr`, `hfsr`, `mmfar`, `bfar`, `exception_lr`,
`exception_msp`, `rtc_status`, `sdram_error` and `uptime_ms`. Reason 0 means normal
startup, 2 a retained fault or explicit recovery, 3 WDT reset, 4 recovery-only
build, 5 boot-flag acknowledgement failure, and 6 SDRAM or control-pointer failure.
The diagnostic payload must be version 3 (`0xe2,3`), exactly 82 bytes.
It contains 20 LE32 values, including `fault_pc`, `stacked_lr`, `stacked_xpsr`,
`control_actual`, `control_expected`, `control_address` and `sensor_step`.
The actual value is captured by the volatile read that failed comparison.
`diagnostic_max_version` is 3; earlier diagnostic formats are rejected.
CRC/session/length/version and field ranges are checked before accepting it;
it never completes or cancels a pending boot/OTA operation. Session changes clear
the old report. The MCU only reads a complete aligned internal MSP exception
frame with no stacking error and a valid Thumb bit. It handles basic and extended
FP frames; PSP, external or invalid frames produce zero appended fields while
preserving recovery and reset. Fault-address registers need their validity bits.

```sh
python3 BRY88BBA15G/tools/backup_screen_boot.py \
  --url http://DEVICE --output /path/to/new-backup
```

Install Python `msgpack` in the shared runtime. If the ESP has an administration
password, set `IOTCLOUD_AUTH=user:password` in the environment. The default is
read-only: no hold, reset, flash erase, protection change or installation.
`--hold --resume` holds a normal startup window and releases it after successful
backup. Omit resume for forced recovery. Existing files are never replaced.
Two complete 81920-byte reads must have identical SHA-256 before the script
writes `boot-backup.json`; interrupted reads and differing copies remain for inspection.
Read-only status/result queries retry transient HTTP 503 and transport failures
up to five attempts. UART requests, resets and installs are never automatically
repeated after a lost response; the script preserves the partial file and stops.
This interface cannot obtain the factory bootloader before first entering a
community application. On this board the minimal application has booted, two
matching loader reads were obtained, and the full application then installed.
The flashing tool selects the stock or community installation path automatically.
It does not require a boot backup or known stock version:

```sh
python3 BRY88BBA15G/tools/flash_screen.py --url http://DEVICE \
  --image full/screen_firmware.bin --mode install \
  --log /path/to/new-install.jsonl
```

After normal business startup, `tools/verify_community_screen.py` checks the
running image, layout ACK and fresh sensors. Its optional `--exercise` sends
temporary external data and tests API/one-shot reminders; it never flashes or
resets either processor. `verify_live_services.py` remains a stock-MCU scenario.

`tools/exercise_screen_recovery.py --url http://DEVICE --image full/screen_firmware.bin
--output /path/to/new-recovery-test` is a separate, explicitly state-changing
development-board test. It resets a matching normal application into forced
maintenance, reads the loader twice, reinstalls that same full image and waits for the startup window
to expire and the previous layout to be acknowledged. Use it only when a screen
reset and installation are intended; it does not test loss of physical power.
`--from-recovery` continues from that same image already in forced maintenance.
Existing reviewed backups can be retained for subsequent application updates.

## TVOC units

The ESP exposes the `tvoc` concentration as ppb, while `screen.status.tvoc_level`
remains the factory screen's independent grade. The factory UART2 request
`11 01 16 D8`, checksum and byte offsets match the manufacturer's
[Cubic AM1002 protocol, section 4.1](https://www.novitronic.com/Datenbl%C3%A4tter/Sensoren/Gassensoren/AM1002.pdf):
`ppb = DF1*256 + DF2`. Original MCU code copies that value to `tvoc`, capped at
2000; there is no additional ppm/mass conversion. This protocol match supports
the unit mapping; it does not establish the exact sensor commercial model.
Only normal status `0x51` and an integer in the factory range are valid.
The screen MCU keeps the recovered hysteresis and excellent/good/poor labels.

## Build and validate

```sh
PHICOMM_FRONTEND_SOURCE=/absolute/path/to/phicomm-community-frontend \
  ~/.cache/phicomm/python3/bin/pio run -d DAIKIN -e bry88bba15g
cmake -S BRY88BBA15G -B ~/.cache/phicomm/bry88-bridge-host \
  -DSCREEN_MCU_SOURCE=/absolute/path/to/iotcloud-bry88bba15g-screenmcu
cmake --build ~/.cache/phicomm/bry88-bridge-host -j8
ctest --test-dir ~/.cache/phicomm/bry88-bridge-host --output-on-failure
```

`DAIKIN/generated/bry88bba15g/frontend.json` records source commit, dirty state,
input hashes, tracked patch hash and final gzip hash. Use the local frontend
override until the new frontend has a published pin.

`tools/verify_live_services.py --url http://DEVICE --evidence DIR` checks queries.
Adding `--exercise --mcu-image FILE` executes the documented config/upload/reboot
scenario on a stock-MCU development board, never MCU activation. It uses the
device's existing empty-password LAN policy and does not bypass authentication.

The frontend's `tests/test_screen_live_browser.cjs` uses `SCREEN_LIVE_URL` and
`SCREEN_BROWSER_ARTIFACTS` to save the same layout at a new revision through the
real browser/device route. It also reads reminders/weather, checks secret
redaction, and captures desktop/mobile screenshots.

Current cross-project results are in the screen repository's
`docs/IMPLEMENTATION-20261105.md` and the private delivery record.

## Appearance / 外观

`screen.appearance.get {}` returns supported/saved/applied revision, error and
the base64 APP1 `bytes`. `screen.appearance.set {bytes}` validates a newer
revision, persists it atomically and sends it only when capability bit 9 is
negotiated. Saving appearance leaves SCN3 untouched. After each MCU session
change the ESP replays it, with at most three attempts and explicit ACK state.

`screen.wallpaper.get {}` reports request/state/code/resource_state/received/
CRC/generation. `begin {header}`, contiguous `chunk {offset,bytes}` (up to 512
decoded bytes), `commit {}` and `cancel {}` are gated by capability bit 11.
Poll `get` for the matching request result before sending the next chunk.
After `begin`, resource state 7 (`preparing`) performs a bounded, incremental
blank-bank scan; keep polling until state 6 before sending chunks. Preparation
has a 60-second overall limit and continues servicing the foreground. Loading
a committed custom image also advances one row per foreground turn, with a
60-second limit. Any I/O, CRC or timeout failure ends the transaction, releases
the module inhibit and falls back to the built-in colors.
Pending or active wallpaper transactions block OTA staging and HTTP image
uploads; OTA similarly blocks wallpaper mutations. No image-sized ESP buffer
or new JSON network endpoint is added.

网页「屏幕 → 外观」支持六套主题、全局自定义、组件五种独立颜色与恢复跟随主题，
以及壁纸、背景透明度、压暗设置。图片可直接选择 PNG/JPEG/WebP，或使用 Pages
前端转换工具生成 `.bwp`；转换仅在浏览器执行。APP1 独立保存与同步，现有布局不变。
外部 NOR 不可用时 MCU 退回纯色，停止重试，串口 OTA 保持可用。
首次上传时状态 7 表示正在分批检查空白资源区，进入状态 6 后才接收图片分片。
检查和图片加载各限时 60 秒；每轮之间继续处理按键、通信和传感器。

Capability bit 12 permits numeric TVOC (ppb) and up to three independently
scaled units in local multi-line charts. The sixth user page can retain its
CO2/TVOC/PM2.5 bindings and disabled flag; no migration rewrites that layout.
The complete negotiated mask is 8189. See the independent screen repository's
`docs/APPEARANCE.md` for APP1/`.bwp` offsets and resource failure rules.

## ESP32 online updates / ESP32 在线更新

`ota.sources.get` returns `github`, `cn`, `global` and the stable/candidate channel.
`ota.check {source}` starts a bounded MessagePack manifest fetch;
`ota.check.get` and `ota.status.get` return state, availability, version and errors.
`ota.install {}` installs only the last validated available offer. Busy OTA,
resource or weather work is rejected for retry. Downloads check the precise
G3/project descriptor, signed envelope, full digest and Flash readback before
selecting the inactive application. Errors keep the current boot partition.
TLS certificate verification is disabled by default; firmware signatures remain
verified. RC builds use `/ota/candidate/bry88bba15g/full/latest.msgpack`.

网页“固件”检查 ESP32 版本；“屏幕 → 资源与更新”检查匹配 MCU 和字库。
屏幕安装仍需本机局域网直连、精确目标和 SHA-256，不写引导区。
