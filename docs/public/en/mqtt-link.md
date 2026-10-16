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

### Pairing

1. On M1, add `A1_MAC:M1_MAC` in binding settings. Both MACs are 12 lowercase hexadecimal digits.
2. On A1, configure the M1 members and automation rules.
3. Check network, MQTT and binding status. Enable automation only after the average readings are valid.

Each identifier authorizes one A1 to query one M1. Adding another M1 does not change existing pairs. M1 accepts at most eight unique identifiers; IP addresses, tokens, recovery intervals and automation rules are not entered there.

### Runtime behavior

A1 initiates session setup, probes and reads. It prefers LAN transport and falls back to MQTT. M1 only answers authorized `bind`, `probe` and `read` requests. Session tokens exist in RAM; A1 periodically establishes a new session after M1 restarts, with no manual token entry.

A1 computes the live arithmetic mean of bound M1 readings that are valid and no older than 15 seconds. With no valid data, readings are `null`. Configure enable switches, thresholds, fan policy, debounce and minimum switching intervals on A1. M1 rejects configuration reads / writes, unbinding and sensor pushes over this channel.

**Upgrade both devices together. Earlier dedicated protocols are not retained.**

## IotCloud remote management

[Open IotCloud]({{SITE_URL}}iotcloud/) · [Deploy IotCloud](iotcloud-deploy)

The [MQTT platform](https://github.com/phicomm-io/phicomm-community-mqtt) provides accounts, device lists and Legacy / TMCP controls. It reuses the local device UI. MQTT initially uses the registration password and can later have a separate password; website and MQTT passwords are managed independently.

Select **China** (`mqtt.fjrcn.cn`) or **US** (`mqtt.fjrcn.com`) at the top. Each node has separate registration, accounts, devices and data. A username on one node does not create an account on the other. Configure the device with the chosen node's Broker and its own credentials.

Login can connect MQTT automatically and reconnect after network loss. You can [prefill a local device](iotcloud-deploy#prefill-a-local-device), review the settings on that device and save. Platform accounts and third-party Broker mode keep device lists and credentials separate. Guest mode provides browser-local device management without a platform login.

See the platform [deployment documentation](https://github.com/phicomm-io/phicomm-community-mqtt#readme) for topic isolation, migrations and permissions. [Online demos]({{SITE_URL}}#demos) use simulated data and do not connect to physical devices.
