# Deploy IoTCloud

To manage devices on the hosted platform, start with [Connect to IoTCloud](iotcloud-connect). This page covers hosting your own services.

[Open IoTCloud]({{SITE_URL}}iotcloud/) · [Frontend source and releases](https://github.com/phicomm-io/iotcloud-community-frontend) · [Backend and Broker deployment](https://github.com/phicomm-io/iotcloud-community-mqtt#readme)

## China and US nodes

| Node | Website / API | Device MQTT | Browser MQTT |
| --- | --- | --- | --- |
| China | https://mqtt.fjrcn.cn | mqtt.fjrcn.cn:1883 | wss://mqtt.fjrcn.cn/mqtt |
| US | https://mqtt.fjrcn.com | mqtt.fjrcn.com:1883 | wss://mqtt.fjrcn.com/mqtt |

Choose a node at the top of IoTCloud. **Accounts, passwords, devices and data are independent. Register separately on each node.** Switching reloads the page and reconnects using the selected node's own session and MQTT credentials; it does not migrate data. Configure devices with that node's Broker hostname and account. The same username on two nodes does not link them.

## Local access without login

[Open local device management]({{SITE_URL}}local/) and enter the device LAN IPv4 address and web administration password. Control the device and configure Wi-Fi and MQTT without an account, backend or Broker. Compatible device firmware and browser local network permission are required. The web password stays in the current page session; enter it again after refreshing.

For standalone static hosting, publish the complete `local/` directory from the frontend release. It skips login and needs no backend address. The regular login page also accepts device IPs. After login, use Settings → Device management → Details to set each LAN address and choose automatic transport or always MQTT. Automatic mode verifies device identity and authentication before using LAN; failed probes keep MQTT available.

## Deployment options

Use [the hosted frontend]({{SITE_URL}}iotcloud/) and choose China / US, host the frontend on GitHub Pages or another static service, or run your own backend and Broker. The frontend repository documents static deployment; the [platform repository](https://github.com/phicomm-io/iotcloud-community-mqtt#readme) documents server initialization and cross-origin access.

## Static frontend

Download `frontend.tar.gz` from a frontend [release](https://github.com/phicomm-io/iotcloud-community-frontend/releases), verify its `SHA256SUMS` and publish the contents of `platform/`. GitHub Pages project subpaths are supported.

For a custom backend, expand **Server settings** on the login page and enter an HTTPS origin without `/api`, plus the MQTT WebSocket address. Alternatively edit the published root `config.json`:

```json
{
  "apiOrigin": "https://api.example.com",
  "mqttWS": "wss://mqtt.example.com/mqtt"
}
```

Only public addresses belong here, never accounts, passwords or tokens. Browser-saved server settings override the site's defaults.

## Backend and MQTT

Initialize the backend and Broker following the platform guide, then configure public HTTPS and WSS. Static hosting serves only the UI; accounts, device synchronization and MQTT still need running server services.

Each node needs its own database, Broker, certificates and secrets. Initialize a new empty runtime directory; do not copy another node's runtime data. For cross-origin frontend access, list the exact trusted origins in `backend/config.yaml`:

```yaml
allowed_origins:
  - https://phicomm-io.github.io
  - https://mqtt.fjrcn.cn
  - https://mqtt.fjrcn.com
```

An origin includes scheme and hostname, plus a non-default port when needed; no `/iotcloud/` path or trailing slash. Set `public_url` to the backend's own public origin. Allow only trusted frontend origins. Platform accounts must use that backend's Broker. Use guest mode for a different Broker. HTTPS pages require WSS.

Device firmware's MQTT Host must be a reachable Broker hostname and device TCP port, not a website URL or WSS path. IoTCloud displays a reminder when the frontend and MQTT hostnames differ.

## Install as a Web App

Use the browser's **Install app** or **Add to home screen**. After a first online visit, the UI is cached and can open offline; login, synchronization and controls still need network access. Confirm the update prompt to switch to a new version.

## Prefill a local device

In **Settings → MQTT connection** or **Device management**, choose **Fill into local device**. Enter the device's LAN IP and verify the Broker hostname, device MQTT TCP port and credentials. A new local device window opens; the public platform server never accesses your LAN.

The device needs a recent WebUI supporting this flow. It displays the requesting platform origin. Click **Receive and prefill**, review the form and **Save** to apply it. Existing device-page authentication remains required. The device never exports its stored MQTT password.

The URL fragment carries only hostname, port, username and one-time handshake metadata, not the password. Password transfer uses window messaging after explicit consent. The handshake expires after two minutes; cancellation or expiry clears unsaved imported data. Verify the destination IP and use a trusted LAN: HTTP cannot prevent eavesdropping or tampering. Blocked popups, isolated windows or unsupported firmware require manual entry; the flow never falls back to putting passwords in a URL.

Dismiss the MQTT hostname reminder with **I understand**; device connection details remain in settings. **Open IoTCloud** on the device opens only the platform entry and never includes credentials.
