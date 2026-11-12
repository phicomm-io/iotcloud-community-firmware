# Show a camera notification when motion is detected

Update both the ESP32 and screen MCU until `screen.push.get` reports
`update_supported` and `policy_supported`. Detection and transcoding run on your
server. The G3 MCU fetches and decodes JPEG/MJPEG in hardware; ESP32 relays bytes.
During motion, renew one notification ID every five seconds. It expires about
15 seconds after the last renewal, including when queued or hidden.

## 1. MJPEG and the webhook bridge

The [Compose example](../examples/motion/compose.yaml) runs go2rtc and a small webhook bridge:

```sh
cd BRY88BBA15G/examples/motion
# Edit the RTSP source in go2rtc.yaml and SERVER_LAN_IP in cameras.json.
export G3_URL=http://G3_LAN_IP
export G3_MOTION_TOKEN=$(python3 -c 'import secrets;print(secrets.token_urlsafe(32))')
# If the G3 has an administration password: export IOTCLOUD_AUTH='admin:password'
docker compose up -d --build
```

Keep the same token when recreating containers and configure it in your automation.
The RTSP input may be a camera substream, Frigate restream, or the RTSP URL
provided by Scrypted's Rebroadcast plugin. Change port 1984 in Compose and
`cameras.json` if an existing go2rtc uses it, or reuse the existing instance.

The sample converts on demand to 320×240 baseline JPEG, 4:2:0, at one frame per
second. [go2rtc FFmpeg](https://github.com/AlexxIT/go2rtc/tree/master/internal/ffmpeg)
supports on-demand conversion and custom encoder templates; its
[MJPEG module](https://github.com/AlexxIT/go2rtc/tree/master/internal/mjpeg) exposes
`/api/stream.mjpeg?src=door_g3`. Adjust both `-r 1` in the template and the G3
`fps` field to change the rate. Start at 1 fps with the default 115200 UART rate.

Use a **server LAN address reachable by the G3** in `cameras.json`, not localhost
or a Docker-only hostname. The source must actually produce the requested size:
G3 width/height parameters and URL placeholders do not transcode arbitrary RTSP
or H.264. Frames must fit within 128 KiB. This example omits audio.

Open the MJPEG URL in a browser, then trigger a notification:

```sh
curl --fail-with-body http://SERVER_LAN_IP:8099/motion \
  -H "Authorization: Bearer $G3_MOTION_TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"camera":"door","active":true}'
```

The reply includes the notification ID and state. Repeated calls renew that ID
without moving the queue entry or reconnecting the source. The bridge converts
JSON webhooks into the device's MessagePack API. It accepts configured camera
names only; callers cannot supply arbitrary destination URLs. Keep ports 1984
and 8099 within your trusted LAN.

## 2. Frigate with Home Assistant

With Frigate MQTT enabled, subscribe to `frigate/door/motion` (`ON` / `OFF`).
`frigate/door/motion/state` indicates whether motion detection is enabled, rather
than current motion. The [official MQTT reference](https://docs.frigate.video/integrations/mqtt/#frigatecamera_namemotion)
also documents `mqtt_off_delay`: the default OFF delay is about 30 seconds,
followed here by the G3's 15-second grace period.

Merge the [Home Assistant example](../examples/motion/home-assistant.yaml) into
your configuration/automation files and replace the camera name and server
address. Set this in `secrets.yaml`:

```yaml
g3_motion_authorization: 'Bearer YOUR_G3_MOTION_TOKEN'
```

ON starts an immediate push and five-second renewals. OFF restarts the automation
and ends at the condition action, stopping the previous loop. Each ON performs
at most 120 renewals, bounding a lost OFF event. Avoid retained historical ON
messages and check actual motion state after reloading automations.

You may reuse Frigate's bundled go2rtc RTSP source or merge the MJPEG template
into that instance. Browser live-view pages may use jsmpeg or WebRTC; G3 requires
a directly accessible JPEG/MJPEG response. See the
[Frigate go2rtc guide](https://docs.frigate.video/guides/configuring_go2rtc/) for
shared sources and transcoding. For people/vehicles only, trigger from Frigate
review/events or the corresponding HA sensor instead of raw motion.

## 3. Scrypted

First check that the camera's MotionSensor changes with real movement. Prefer
native camera events. Otherwise, consult [Scrypted motion detection](https://docs.scrypted.app/detection/motion-detection.html),
which distinguishes OpenCV from accelerated detection requiring Scrypted NVR.

Create a script in the Scripts plugin and paste
[scrypted-motion.ts](../examples/motion/scrypted-motion.ts). Fill in the camera
device ID, webhook URL and token. It listens to MotionSensor and renews every
five seconds during motion, stopping after motion ends or ten minutes without a
new event. It waits for a fresh event at startup instead of replaying old state.
The interface is documented at [ScryptedDevice.listen](https://developer.scrypted.app/gen/interfaces/ScryptedDevice.html#listen).

Alternatively, publish camera events with the official
[MQTT plugin](https://github.com/koush/scrypted/tree/main/plugins/mqtt) and build an
HA automation like the Frigate example. Inspect that plugin's actual topic and
payload; Frigate topic names do not apply. The Scrypted Webhook plugin primarily
invokes Scrypted devices; use a script or automation for outbound notification
requests. These integration methods are listed in [Scrypted Development](https://developer.scrypted.app/).

Use the Rebroadcast plugin's RTSP restream as go2rtc input. The Scrypted management
URL or a WebRTC page is not an MJPEG source.

## 4. IDs, expiration and overflow

The bridge generates an ID for each new event and renews only these fields:

```json
{"action":"screen.push.update","params":{"id":2001,"duration_ms":15000,"ttl_ms":15000}}
```

`duration_ms` is remaining visible time and can only be extended. `ttl_ms` resets
a hard lifetime from receipt, including queue/hidden/confirmation time; 0 disables
it and the maximum is one day. Even confirmation notices expire with a TTL.

Each `screen.push.send` can choose an overflow policy:

| Policy | Full queue behavior |
| --- | --- |
| `reject` (default) | Reject, preserving the queue; retry once space is available |
| `drop_newest` | Accept and discard the incoming notice; record `dropped` |
| `drop_oldest` | Evict the oldest waiting notice; record `evicted`, protecting the current notice |

The bridge uses `drop_oldest`. The MCU enforces the last TTL if the bridge or
automation stops. Visible time completion is `timedout`; hard expiry is `expired`.
Confirmation, explicit dismissal and decode failure have separate states.
Inspect `queue`, `messages` and `request` in `screen.push.get` for the final MCU
result after HTTP acceptance.

## 5. Checks

- Webhook 401: check the Bearer token. 503: check device connectivity, both
  firmware capabilities and the MCU receipt. After a lost reply, the bridge
  queries its current ID before creating anything new.
- Loading or source_error: GET the MJPEG URL from the G3 LAN and check baseline
  JPEG, 4:2:0, dimensions, frame size, credentials and certificates.
- Delay after motion ends: check Frigate OFF delay and the grace period; verify
  that `expires_in_ms` decreases.
- The queue holds 16 notices and four uploaded images. URL notices share one
  fetch/decode owner; updating a hidden notice preserves the current view.

After deployment, test real motion, continuous renewal, OFF/expiry, and expiration
while the bridge is stopped.
