# G3 weather, reminders and external data

Install [current compatible firmware]({{SITE_URL}}en/#releases), then configure Weather, Reminders and External data under Settings → Screen. See [layouts](daikin-g3-layout) for composition and the [main guide](daikin-g3-guide) for installation. ESP32 runs network services and sends normalized data to the screen MCU.

## Weather

| Provider | Configuration |
| --- | --- |
| Open-Meteo | No key for personal, noncommercial use; coordinates and a location label |
| QWeather | Your API key and project host ending in `.qweatherapi.com` |
| Seniverse | Your API key with access to the required APIs |
| OpenWeather | Your API key; metric units |

For Guangzhou, Guangdong, use latitude `23.1291`, longitude `113.2644` and label “Guangzhou”. A 900-second interval is recommended; the range is 300–86400 seconds. The label is display text; coordinates determine the queried location. Open-Meteo retrieves current conditions and three forecast days in one request. Choosing it clears the previous provider's key and custom host. Other providers require your own key rather than a shared private credential.

Server-certificate verification defaults to off and is saved per device; enable it explicitly when wanted. Requests still use HTTPS, and this switch does not alter firmware signature/hash checks. Normalized weather codes select icons and Chinese/English descriptions. Chinese city labels require the full font to finish loading.

“Expired” means the cache has exceeded its validity period, not that the city was unrecognized. Check enablement, coordinates, key/host, device time, last error and retry countdown, then request a refresh. Failures preserve the previous content and back off for 30, 60, 120, 240 and 300 seconds. Workers never overlap; manual refresh also requires 15-second spacing.

TLS needs byte-addressable heap and a contiguous block, so total ESP32 free memory alone is insufficient. Layout/resource uploads, OTA and weather have concurrency gates; memory pressure defers retries. See [memory diagnostics](daikin-g3-diagnostics#esp32-memory-and-weather).

## Reminders

Up to 32 scheduled rules and 32 notices persist. Rules support a one-shot UTC epoch, daily local time or weekly local time; Monday is weekday bit 0. The fixed minute-offset timezone matches the screen, with `480` for Beijing time. Boot and large clock jumps do not replay the entire backlog.

Notices persist, including read acknowledgements. Wake policies obey DND, always wake or wake only for unread notices; individual rules may inherit the global policy. Titles allow 64 UTF-8 bytes and body text 127 bytes, counting bytes rather than characters.

## API and external charts

Use authenticated `POST /api/v1/command`, `Content-Type: application/msgpack`, with `{request_id, action, params}`. Braces below describe objects before encoding; do not send them as JSON to this interface. MQTT/LAN share the business actions. Query capabilities first.

| Action | Example parameters or purpose |
| --- | --- |
| `screen.weather.get` / `.refresh` | Read redacted configuration/status or schedule a refresh |
| `screen.reminders.configure` | `{timezone:480,wake:0}` |
| `screen.reminders.set` | `{id:1,kind:2,minute:540,days:31,wake:255,enabled:true,title:"Ventilate",text:"Open the window"}`; kind 0 once, 1 daily, 2 weekly |
| `screen.reminders.push` | `{title:"Visitor",text:"Doorbell pressed",wake:0}`, returning a notice ID |
| `screen.reminders.ack` / `.delete` | `{id:...}` to mark a notice read or delete a scheduled rule |
| `screen.page.set` | `{direction:"next"}`, `{direction:"previous"}` or `{page:3}`, with one-based pages |

External pushes support values, text and charts with source, name, unit and TTL. Expired data must not remain a valid reading. Charts allow three series of 32 points, `null` gaps, finite values with magnitude at most `1e9` and TTL 1–86400 seconds. Complete parameters, chunked layouts, resource formats and acknowledgements are documented in the [service protocol](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/SERVICES.md).

See [device interfaces and protocols](device-api) for authentication and encoding examples. An API read, ESP32 queue admission, MCU acknowledgement and physical rendering are separate stages. Poll matching request IDs where supplied.
