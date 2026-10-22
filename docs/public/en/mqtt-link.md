# MQTT and automation

MQTT is optional: local device functions work without it. Configure a Broker address, port, username and password in the device WebUI to connect Home Assistant or IotCloud.

## Protocol modes

| Device | Supported mode |
| --- | --- |
| TC1 | Legacy (v1) and TMCP (v2); TMCP is the default for a new setup |
| M1 / A1 | TMCP (v2) only; no Legacy selector |

M1 and A1 normalize old mode flags to TMCP at boot, retaining the Broker address, port and credentials. They do not switch back to old topics at runtime.

Device topics use `phicomm/v2/<scope>/<model>/<mac>/...`. A nonempty username maps to `u_<lowercase hexadecimal UTF-8 bytes>`; an empty username uses `local` with the same v2 protocol. Home Assistant discovery uses `homeassistant/<component>/<scope>/<model>_<mac>_<entity>/config`.

**A topic prefix is not an authorization boundary. Broker ACLs are required for account isolation.**

## Home Assistant

Configure the Home Assistant MQTT integration to use the same Broker. Device readings and controls appear through MQTT discovery. M1 and A1 can subscribe read-only to Home Assistant startup notifications to refresh discovery after HA restarts.

## A1 and M1 automation

M1 measures, reports and answers authorized A1 requests. It does not control the purifier. Rules, aggregation, fan, ring and group controls live on A1.

### Setup

1. On A1, add the M1 MAC, optionally its IP, and configure automation rules. Each A1 supports up to four M1 units.
2. Connect both devices to the same MQTT service and account. The Broker must allow that account to publish to the M1 command topic.
3. Confirm LAN connectivity and valid average readings before enabling automation. M1 needs no pairing identifier; its page shows current A1 sessions.

With IP left blank, A1 broadcasts on the LAN to find the specified M1 MAC. Enter a reachable IP when crossing subnets or broadcasts are restricted.

### Runtime behavior

A1 sends a session token to M1 through MQTT, then queries readings over UDP every five seconds. Healthy reads do not repeat authorization or forward sensor data through MQTT. UDP failures use retry backoff; unavailable readings trigger A1's configured offline fan speed.

Session tokens exist only in RAM. M1 supports four A1 sessions, expiring after thirty minutes idle. A restart requires MQTT authorization again; A1 retries automatically, with authorization limited to once per minute during failures. Established sessions with continuing reads survive a temporary MQTT outage.

A1 computes the arithmetic mean of M1 readings no older than fifteen seconds. With no valid data, readings are `null`. Configure thresholds, fan policy, debounce and minimum switching intervals on A1. A1 group control remains independent.

**Upgrade both devices together. The old M1 allowlist and joint pairing topics are no longer used.**

## IotCloud remote management

[Connect devices to IotCloud](iotcloud-connect) · [Open IotCloud]({{SITE_URL}}iotcloud/) · [Deploy IotCloud](iotcloud-deploy)

The [MQTT platform](https://github.com/phicomm-io/phicomm-community-mqtt) provides accounts, device lists and Legacy / TMCP controls. It reuses the local device UI. MQTT initially uses the registration password and can later have a separate password; website and MQTT passwords are managed independently.

Select **China** (`mqtt.fjrcn.cn`) or **US** (`mqtt.fjrcn.com`) at the top. Each node has separate registration, accounts, devices and data. A username on one node does not create an account on the other. Configure the device with the chosen node's Broker and its own credentials.

Login can connect MQTT automatically and reconnect after network loss. You can [prefill a local device](iotcloud-deploy#prefill-a-local-device), review the settings on that device and save. Platform accounts and third-party Broker mode keep device lists and credentials separate. Guest mode provides browser-local device management without a platform login.

See the platform [deployment documentation](https://github.com/phicomm-io/phicomm-community-mqtt#readme) for topic isolation, migrations and permissions. [Online demos]({{SITE_URL}}#demos) use simulated data and do not connect to physical devices.
