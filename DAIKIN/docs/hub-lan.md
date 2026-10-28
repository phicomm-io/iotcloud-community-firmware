# Hub LAN transport

The hub discovers `_openiot._udp.local.` services with explicit `id`, `model`, `variant`, `firmware`, `lan_wire=4` and `codec=msgpack` TXT fields. Missing or invalid fields never select a guessed codec. Discovery is not authentication or accessory registration. The query uses the nonblocking IDF asynchronous API, preserving local control processing.

There is no embedded MQTT broker, MQTT WebSocket server, retained LAN message store, or internal device MQTT connection. Existing readable route keys remain only as application routing keys for the external MQTT/Bemfa integration. The upstream MQTT client, independent HA JSON publications to an external broker, Bemfa and HomeKit remain supported. Legacy cloud JSON/new cloud MP codec selection stays separate from native LAN MP.

A device can receive LAN commands only after explicit owner registration, CONTROL pairing, matching advertised model/identity and confirmed paired address. An untrusted discovery address cannot silently replace a paired route. An address change is shown as `route_changed`; explicit pairing to the new address updates the pin. Credentials are not exposed in inventory or snapshots.

`GET /api/hub/lan` exposes derived inventory through the existing hub owner session. `POST /api/hub/lan/pair` accepts `{peer,ip,password,scopes:1}`, `GET /api/hub/lan/pair/<peer>` reports pairing progress, and `POST /api/hub/lan/unpair` requests revocation. All require owner authentication. Discovery and pairing alone do not register a HomeKit accessory.

Native UDP v4 uses the shared `common/lan_wire_v4` contract. A request is at most 1024 bytes, a frame at most 1200, and a response at most 8192. Responses are pulled in ordered 768-byte pieces and acknowledged. A single in-flight hub exchange owns the original request bytes and response buffer. Retries preserve bytes, boot, token and ID; there is no fallback or fresh-ID automatic write replay. Response expiry, restore/reload, credential changes and unpair cancel local waiting without rewinding the ID counter. The current token, epoch, pairing phase, registration and route are checked again before accepting receipts or retrying.

Periodic `status.get` runs independently of HomeKit being enabled. Only a matching successful status receipt's full `data` object becomes a measured `/state` event for HomeKit/Bemfa/upstream routes. A mutation receipt alone is never a measured state. HomeKit still requires its receipt followed by a fresh state confirmation. While waiting, local GPIO, sensors, indicators and the business cache keep running; HomeSpan is not recursively polled.

The DAIKIN device server permits CONTROL-scoped status/capabilities reads and indicator/CO2 calibration business actions. It does not grant configuration, password, reset or reboot privileges to CONTROL tokens. Existing v3 sensor-session authorization and password pairing remain available.

Host evidence includes actual localhost UDP through the production client/shared server plus production Home model; token authentication and physical TC1 business are explicit fixtures. Tests cover chunk loss/retry, ACK-before-state rejection, no duplicate mutation, wrong identity/token/port and revocation. These are not DUT, real mDNS network, physical HomeKit pairing, production cloud, or pressure-test acceptance. Those gates remain required before deployment.
