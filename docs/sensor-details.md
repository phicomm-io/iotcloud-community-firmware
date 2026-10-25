# M1 颗粒物详情

MCU 的 OP00 能力应答必须声明 sensor schema=1、种类数至少 7、块大小 9；不使用版本字符串推测支持。联网模块在原 UART 所有者中异步读取 OP10，HTTP/MQTT 只读取本地快照，不阻塞等待传感器。

每轮读取 PM 和一种轮换的诊断帧，与既有 CO₂/音乐查询共用调度。单帧按“元数据 → 每块最多 9 字节 → 再读元数据”获取，核对 ID、偏移、长度和代数；代数改变、传输错误或超时会立即使该帧健康状态失效，下轮重新获取。MCU 启动事件和升级开始清除缓存与能力。

## HTTP / TMCP

- `POST /api/v1/command`，`{"action":"sensor_details.get","params":{}}`。
- `GET /api/v1/sensor_details`，使用相同鉴权及服务分发。
- `capabilities.get.actions` 中公开 `sensor_details.get`。旧 MCU 返回 `supported=false, available=false`；旧联网固件没有此 action，前端隐藏卡片。
- 数据可用需同时满足：完整 32 字节 PM 帧、头/长度/求和校验通过、健康为 1、已缓存且校验通过、无传感器错误/预热/过滤标志、错误字节为 0、MCU 帧龄加本地经过时间小于 6000 ms。
- `available` 表示当前可用；`supported` 表示能力支持；`fresh`、`healthy` 提供独立状态。未收到帧时 `age_ms=null`。不可用时省略整个 `pm` 对象，不能用零值填充；合法的零测量保持为零。
- `pm.atmospheric_ug_m3` 和 `pm.cf1_ug_m3` 分别包含 `pm1/pm25/pm10`；`pm.particles_per_0_1_l` 包含 `gt_0_3/gt_0_5/gt_1_0/gt_2_5/gt_5_0/gt_10_0`。
- `diagnostics` 分别保留 `pm/hcho/sht/sht_status/sht_aux/hcho_probe/pm_probe` 的 `captured/raw_hex/generation/flags/health/age_ms`，不将甲醛未知字节或 SHT 辅助字节命名为测量值。每种诊断帧有自己的时间与代数，不宣称跨传感器同步采样。

## MQTT / Home Assistant

新增 `<device base>/particles`，每 3 秒发布当前状态，**不保留**，使用上述测量字段，不附带原始诊断帧。即使不可用也发送状态，避免旧读数冒充有效值；分配失败有周期退避。原有 `sensor` 消息及 PM2.5 实体保持原契约。

新增 PM1.0、PM10 与六档累计颗粒数共八个 Discovery 实体，浓度使用大气环境字段，颗粒数单位 `#/0.1 L`。同时检查设备 availability 和 particles 中的 available，并设置 6 秒过期。支持能力变化或 HA birth 后重新发现；不支持时删除对应保留 Discovery 配置。配置清理包含全部新实体。

## 前端与验证

源码位于相邻 `phicomm-community-frontend` 仓库的 `M1/web/page.js`。设备 WebUI 与平台内嵌首页复用独立“颗粒物详情”卡片：PM1.0、PM10、六档颗粒数，折叠查看 CF=1 的额外浓度和原始诊断；PM2.5 继续使用原主读数。浏览器将响应帧龄加上本地经过时间，到期显示“—”，缓存页面恢复后立即重新核对有效期。

测试入口：`tests/test_m1_sensor_details.c`、`tests/test_stock_runtime.py`、`tests/test_m1_particles_mqtt.py`；前端 `tests/test_m1_particles.cjs` 和 `tests/test_m1_particles_browser.cjs`。

2026-10-25 已在 M1 `b0f8932864b6` 构建并 OTA 验证详情 API、390/1280 像素页面与配置保留。测试机 MCU 为声明本能力的开发版 `0.9.3-2df7a1-d`，未改写 MCU；不将该结果表述为正式 MCU 0.9.4 的刷机验收。旧 MCU、失效数据、严格 JSON 下未采集帧龄为 null、八个 Discovery 配置通过主机回归；实际 Home Assistant 实例接入尚未验证。

本地开发可使用 `PHICOMM_FRONTEND_SOURCE=/absolute/path/phicomm-community-frontend ./M1/build.sh` 生成含新卡片的候选镜像。固件通过 `frontend.lock.json` 锁定前端的提交和归档校验和。按本次要求只提交至 main，不创建固件 tag 或 GitHub Release。
