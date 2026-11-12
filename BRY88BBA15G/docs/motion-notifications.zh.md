# 运动检测后在 G3 显示视频

使用已支持 `update_supported` 和 `policy_supported` 的 ESP32、屏幕固件。
检测和视频转码放在服务器；G3 MCU 拉取 JPEG/MJPEG、硬件解码并显示，ESP32 透传网络数据。
运动期间每五秒更新同一通知 ID，运动停止后约 15 秒关闭。通知排队或被隐藏时也会过期。

## 1. 准备 MJPEG 和推送桥

[完整容器示例](../examples/motion/compose.yaml) 使用 go2rtc 和一个轻量 webhook 桥：

```sh
cd BRY88BBA15G/examples/motion
# 修改 go2rtc.yaml 的 RTSP 源、cameras.json 的 SERVER_LAN_IP。
export G3_URL=http://G3_LAN_IP
export G3_MOTION_TOKEN=$(python3 -c 'import secrets;print(secrets.token_urlsafe(32))')
# G3 设置过管理密码时：export IOTCLOUD_AUTH='admin:管理密码'
docker compose up -d --build
```

保留 token 供自动化使用；容器重建时继续传入同一个 token。RTSP 可来自摄像头子码流、Frigate 的 RTSP restream，或 Scrypted Rebroadcast 插件提供的 RTSP 地址。示例用独立端口 1984；已有 go2rtc 占用时改端口和 `cameras.json`，或直接复用现有实例。

go2rtc 配置把视频缩放到 320×240、每秒一帧、baseline JPEG 4:2:0，收到观看请求后才启动 FFmpeg。[go2rtc FFmpeg](https://github.com/AlexxIT/go2rtc/tree/master/internal/ffmpeg) 支持按需转码和自定义编码模板；[MJPEG 模块](https://github.com/AlexxIT/go2rtc/tree/master/internal/mjpeg) 提供 `/api/stream.mjpeg?src=door_g3`。修改模板的 `-r 1` 和 G3 的 `fps` 可调整帧率；先从一帧开始，默认 115200 链路的吞吐较低。

`cameras.json` 的 URL 必须是 **G3 能访问的服务器局域网地址**，不能使用 `localhost` 或仅容器可解析的服务名。摄像头画面需由服务端提供指定大小：G3 的 `width`、`height` 和 URL 占位符不会把任意 RTSP/H.264 自动变成低分辨率 JPEG。每帧须不超过 128 KiB；本示例不转发音频。

在浏览器打开 MJPEG 地址检查视频，再测试 webhook：

```sh
curl --fail-with-body http://SERVER_LAN_IP:8099/motion \
  -H "Authorization: Bearer $G3_MOTION_TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"camera":"door","active":true}'
```

返回 `id` 和通知状态，再次调用会保留该 ID、图源连接和队列位置并续期。桥将 JSON webhook 转成 G3 的 MessagePack 接口；设备接口保持 MessagePack。桥只接受配置中的摄像头名称，不能通过 webhook 指定任意目标地址。1984/8099 仅供受信任局域网使用。

## 2. Frigate + Home Assistant

Frigate 启用 MQTT 后，使用 motion 事件 `frigate/door/motion` 的 `ON` / `OFF`。`frigate/door/motion/state` 表示检测功能是否启用，不能用来判断当前有没有运动。[Frigate 官方 MQTT 说明](https://docs.frigate.video/integrations/mqtt/#frigatecamera_namemotion) 还说明了 `mqtt_off_delay`；默认 OFF 延迟约 30 秒，G3 的 15 秒尾随时间从自动化停止续期后再计算。

把 [Home Assistant 示例](../examples/motion/home-assistant.yaml) 合并到对应的配置和自动化文件，替换服务器地址、摄像头名称。在 `secrets.yaml` 设置：

```yaml
g3_motion_authorization: 'Bearer 替换为相同的G3_MOTION_TOKEN'
```

示例在 ON 时立即推送并每五秒续期；OFF 会重启自动化并在条件动作处结束，停止旧循环。单次 ON 最多续期 120 次，防止丢失 OFF 后无限保持。保持 motion 主题不保留历史 ON 消息；自动化重载后检查实际运动状态。

已有 Frigate 自带的 go2rtc 时，可以复用 RTSP 源，或把示例的 MJPEG 转码模板合并到现有 go2rtc 配置。Frigate 的视频预览格式可能是 jsmpeg/WebRTC，G3 需要可直接 GET 的 JPEG/MJPEG。[Frigate go2rtc 配置指南](https://docs.frigate.video/guides/configuring_go2rtc/) 说明了流共享与 FFmpeg 转码；不要把浏览器预览页 URL 当成视频流地址。

只想推送“人/车”等对象时，可改用 Frigate review/events 或 HA 对应传感器；仍调用同一 webhook，避免把原始 motion 当成人体识别结果。

## 3. Scrypted

先在 Scrypted 确认摄像头有可变化的 MotionSensor。优先使用摄像头原生运动事件；没有时参考 [Scrypted 运动检测](https://docs.scrypted.app/detection/motion-detection.html)。官方区分普通 OpenCV 插件与需要 Scrypted NVR 的加速检测。

在 Scripts 插件新建脚本，粘贴 [scrypted-motion.ts](../examples/motion/scrypted-motion.ts)，填写摄像头设备 ID、webhook 地址和 token 后运行。脚本监听 `MotionSensor`，运动期间每五秒续期，停止后不再发送；一次事件最长续期十分钟。脚本启动时等待新事件，不重播启动前残留状态。事件监听接口见 [ScryptedDevice.listen](https://developer.scrypted.app/gen/interfaces/ScryptedDevice.html#listen)。

也可以使用官方 [MQTT 插件](https://github.com/koush/scrypted/tree/main/plugins/mqtt) 发布摄像头状态，在 HA 建立与上例相同的自动化。实际 MQTT topic 和 payload 从该插件导出设备的配置/消息确定，不套用 Frigate 的主题。Scrypted Webhook 插件主要用于调用 Scrypted 设备；此处向外发送推送由脚本或自动化完成。[Scrypted 开发入口](https://developer.scrypted.app/) 列出了这些方式。

视频地址使用 Rebroadcast 插件的 RTSP restream 填入 go2rtc；Scrypted 管理页、WebRTC 页面不是 G3 的 MJPEG 源。本例无需修改现有 Scrypted 账号、插件和录像配置。

## 4. 通知 ID、过期与队列策略

桥为新事件生成 ID；后续心跳使用 `screen.push.update`，只改两个时长字段：

```json
{"action":"screen.push.update","params":{"id":2001,"duration_ms":15000,"ttl_ms":15000}}
```

`duration_ms` 是实际显示的剩余时长，只能延长；`ttl_ms` 是从本次接收起的硬过期时间，更新时重设，0 关闭。其上限为一天，即使确认型通知也会到期。

`screen.push.send` 可设置 `overflow`：

| 策略 | 队列满时的结果 |
| --- | --- |
| `reject`（默认） | 拒绝，队列不变；空出位置后可重新提交 |
| `drop_newest` | 接收并主动丢弃新消息，记录 `dropped` |
| `drop_oldest` | 淘汰最早的等待消息，记录 `evicted`，保留当前显示项 |

桥选择 `drop_oldest`；没有运动心跳、桥退出或自动化中断后，最后一次续期的 TTL 仍由 MCU 执行，旧通知不会永久占位。自然显示结束是 `timedout`，硬过期是 `expired`；确认、主动关闭、解码失败也各有独立状态。查询 `screen.push.get` 的 `queue`、`messages` 和 `request` 检查最终结果；HTTP 接收成功后还应检查 MCU 回执。

## 5. 排查

- webhook 返回 401：核对 Bearer token；503：检查设备在线、两端固件能力和 MCU 回执。桥在丢失回执后查询同一 ID，不自动重放一条新消息。
- 只有加载提示或 source_error：从 G3 同一局域网 GET MJPEG 地址，核对 baseline、4:2:0、分辨率、帧大小、认证和证书。
- 运动结束后仍短暂显示：检查 Frigate OFF 延迟和 15 秒尾随时间；查看 `expires_in_ms` 是否递减。
- G3 队列仍为 16 条、图片缓存为 4 张。URL 通知共享单个拉流/解码器；更新隐藏通知不会抢占当前画面。

部署后验证一次真实运动、一次持续运动续期、一次 OFF/过期，以及桥停机时通知仍会消失。
