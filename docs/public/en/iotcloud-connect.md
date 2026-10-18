# Connect devices to IotCloud

IotCloud is a web management platform for the TC1 power strip, M1 air quality monitor and A1 air purifier. Connect devices to view online status, air readings and power usage in one place, and remotely use each device's supported controls and settings. Account mode saves the device list so you can continue from another phone or computer on the same node and account.

[Open IotCloud]({{SITE_URL}}iotcloud/) · [Try the simulated demo]({{SITE_URL}}demos/platform/)

## 1. Prepare the device and account

Install this project's firmware, configure Wi-Fi and confirm that the device WebUI opens at its LAN IP. See [Quick start](quick-start) for installation. During configuration, your phone or computer must be able to reach the device's LAN. Remote management requires both the device and browser to reach the selected platform node.

Open IotCloud, select China or US at the top, then choose registration. Set a username and password and complete email verification if requested. If registration is closed, follow the page's administrator contact instructions.

| Node | Website | Device Broker host | Device MQTT port |
| --- | --- | --- | --- |
| China | [mqtt.fjrcn.cn](https://mqtt.fjrcn.cn/) | `mqtt.fjrcn.cn` | `1883` |
| US | [mqtt.fjrcn.com](https://mqtt.fjrcn.com/) | `mqtt.fjrcn.com` | `1883` |

**Accounts, passwords, devices and data are independent on the two nodes. Register separately.** Use the same node for registration, login and device configuration. Switching nodes does not migrate devices.

After login, open Settings → MQTT connection, select the platform Broker and confirm that it is connected. Connection is automatic by default; if you previously disconnected manually, click the connect button.

> The initial MQTT password is the registration password. Website and MQTT passwords can later be changed independently. Changing the website password does not change the MQTT password. After changing the MQTT password, update the browser connection and every device. The device's local WebUI password is separate from MQTT authentication.

## 2. Connect the device

Choose either method below, using the same Broker and account details.

### Option A: Enter settings manually

1. Open the device's LAN IP in your browser and enter its WebUI password if required.
2. Open Network → MQTT / Home Assistant and fill in the Broker form:

   | Field | Value |
   | --- | --- |
   | Broker address / MQTT Host | Selected node's hostname, for example `mqtt.fjrcn.cn` |
   | Port | `1883` |
   | Username | Platform username registered on that node |
   | Password | Current MQTT password; initially the registration password |
   | Topic mode | On TC1, select **TMCP (v2)** under Advanced. M1 and A1 always use TMCP |

3. Save and wait for the device's MQTT status to show connected. If it keeps reconnecting, check the host, username and password.

**Enter only the hostname in the device Broker field, without `https://`, `wss://` or `/mqtt`.** `https://phicomm-io.github.io/iotcloud/` is the web entry, not a device Broker. The browser addresses `wss://mqtt.fjrcn.cn/mqtt` and `wss://mqtt.fjrcn.com/mqtt` must not be used as device Hosts either.

### Option B: Prefill from IotCloud

1. On a phone or computer that can reach the device's LAN, open IotCloud Settings → MQTT connection or Device management and choose the option to fill settings into a local device.
2. Enter the device IP or local URL. Check the Broker hostname, TCP port `1883`, platform username and current MQTT password, confirm the destination and open the device window.
3. In the new device window, check the sending platform's origin and accept the prefill request. The device's existing WebUI password still applies.
4. Review the prefilled values, click Save on the device page and confirm MQTT is connected. A platform message that values were prefilled only confirms delivery to the device WebUI.

Keep the platform window open and accept within two minutes. Send only to a verified device address on a trusted LAN. If popups are blocked, the request expires or the device WebUI does not support this feature, enter settings manually.

## 3. Confirm the device is online

Return to IotCloud on the same node. TMCP devices using the same MQTT account appear automatically. Wait for the list to refresh, or open Settings → Device management and click Refresh.

If a device is missing, first check its local MQTT status. You can also use Device management → Add device, entering the **MAC address** shown in its WebUI, the correct model and protocol. MAC addresses have 12 hexadecimal characters; colon or hyphen separators are also accepted. Adding a device to the list does not configure its MQTT connection.

- **TC1:** TMCP is recommended for this project's firmware. Legacy (v1) is for TC1 devices actually using the old protocol and requires manual addition.
- **M1 and A1:** Select the correct model; the protocol is always TMCP.

Confirm that platform MQTT is connected, the device card is online and readings keep updating. Open an M1's live readings, or adjust its screen brightness and check the physical device. Check other devices' functions as needed. Once connected, you can switch your phone to mobile data and manage online devices without forwarding ports to them.

See [TC1](tc1-guide), [M1](m1-guide) and [A1](a1-guide) for daily operation. A1 / M1 permissions and automatic linkage must still be configured separately using [MQTT and automation](mqtt-link).

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Website login works but platform MQTT is disconnected | Select the platform Broker in Settings → MQTT connection, check the current MQTT password and connect. Website login does not confirm MQTT connectivity |
| Platform MQTT is connected but the device is offline | Check local Wi-Fi and MQTT status, Broker host, port `1883`, node account and current MQTT password; confirm device internet access |
| Device is not discovered | Use the same node and MQTT username on both sides; select TMCP on TC1. Legacy TC1 requires manual addition |
| Manually added device stays offline | Check MAC, model and protocol, and save MQTT settings on the device itself |
| Connection fails after a password change | Distinguish website and MQTT passwords; update the browser and all devices after changing the MQTT password |
| Device list is empty after changing nodes | Nodes have separate accounts and devices. Return to the original node, or register and reconfigure devices on the new one |
| Using your own MQTT server | Choose self-hosted Broker / guest mode and enter a WebSocket-capable address. Its device list is separate from platform account mode |

For hosting your own frontend, account service or Broker, see [Deploy IotCloud](iotcloud-deploy).
