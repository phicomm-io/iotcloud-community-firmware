# MQTT and automation

MQTT is optional: local device functions work without it. Configure a Broker address, port, username and password in the device WebUI to connect Home Assistant or IoTCloud.

## Protocol modes

| Baseline | Device protocol |
| --- | --- |
| **RC5** | TC1, M1, A1, DAIKIN and S30M-V1B use the openiot protocol with TMCP v2 command envelopes and MessagePack HTTP / MQTT payloads |
| **0.9.27** | JSON HTTP / TMCP; TC1 supports Legacy v1 or TMCP v2, M1 / A1 / DAIKIN use TMCP v2; S30M-V1B is not included |

Check the firmware version and encoding before integrating a custom client. See [device interfaces and protocols](device-api) for fields, topics and examples. Home Assistant uses its separate JSON messages.

Device topics use `phicomm/v2/<scope>/<model>/<mac>/...`. A nonempty username maps to `u_<lowercase hexadecimal UTF-8 bytes>`; an empty username uses `local` with the same v2 protocol. Home Assistant discovery uses `homeassistant/<component>/<scope>/<model>_<mac>_<entity>/config`.

**A topic prefix is not an authorization boundary. Broker ACLs are required for account isolation.**

## Home Assistant

Configure the Home Assistant MQTT integration to use the same Broker. Device readings and controls appear through MQTT discovery. M1, A1 and DAIKIN can subscribe read-only to Home Assistant startup notifications to refresh discovery after HA restarts.

## A1 sensor automation

M1, DAIKIN and other providers measure, publish and answer authorized reads. Rules, aggregation, fan, ring and group control live on A1. Available fields determine automation choices independently of the provider model.

1. Add up to four provider MACs and optional IPs on A1; different monitor models can be mixed.
2. Use the same MQTT service and account. Update the platform to support generic sensor topics. A self-hosted Broker must permit measurements at `phicomm/v2/<scope>/sensor/<mac>/state` and session grants through its matching `command` topic.
3. Confirm valid readings before enabling automation. A monitor lists an A1 as connected only after an actual successful read.

Without an IP, A1 can broadcast on the LAN to find the specified MAC. Enter a reachable address across subnets or when broadcasts are restricted. A1 sends a session token through non-retained MQTT, then queries UDP every five seconds. LAN readings take priority; generic MQTT measurements can be used when LAN reads are unavailable.

Tokens exist only in RAM. Each monitor supports four A1 sessions, expiring after thirty minutes idle. After a monitor restarts, A1 automatically reauthorizes through MQTT. Established sessions with continuing UDP reads survive a temporary MQTT outage.

Each field has independent unit, validity and age checks. PM2.5, formaldehyde, temperature and humidity expire after fifteen seconds; CO₂ expires after twenty seconds. Boot identifiers and sequence numbers reject old or repeated samples; replay does not renew freshness. Statistics include only providers with valid data for that metric; no valid readings means `null`. Missing or stale data hides the corresponding choices while preserving saved rules. Unavailable required data invokes A1's offline speed. PM10 and TVOC are not currently fan-control metrics.

Configure thresholds, statistical strategy, debounce and minimum switching intervals on A1. Group control remains independent. Upgrade both sides together; the old M1 allowlist and joint pairing topics are no longer used.

## IoTCloud remote management

[Connect devices to IoTCloud](iotcloud-connect) · [Open IoTCloud]({{SITE_URL}}iotcloud/) · [Deploy IoTCloud](iotcloud-deploy)

The [MQTT platform](https://github.com/phicomm-io/iotcloud-community-mqtt) provides accounts, device lists and Legacy / TMCP controls. It reuses the local device UI. MQTT initially uses the registration password and can later have a separate password; website and MQTT passwords are managed independently.

Select **China** (`mqtt.fjrcn.cn`) or **US** (`mqtt.fjrcn.com`) at the top. Each node has separate registration, accounts, devices and data. A username on one node does not create an account on the other. Configure the device with the chosen node's Broker and its own credentials.

Login can connect MQTT automatically and reconnect after network loss. You can [prefill a local device](iotcloud-deploy#prefill-a-local-device), review the settings on that device and save. Platform accounts and third-party Broker mode keep device lists and credentials separate. Guest mode provides browser-local device management without a platform login.

See the platform [deployment documentation](https://github.com/phicomm-io/iotcloud-community-mqtt#readme) for topic isolation, migrations and permissions. [Online demos]({{SITE_URL}}#demos) use simulated data and do not connect to physical devices.
