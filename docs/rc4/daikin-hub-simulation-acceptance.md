# DAIKIN rc.4 hub simulation acceptance

Production snapshot: DAIKIN at `af03637`, byte-identical to root `05c328c`.
This change adds tests and CI wiring; it does not alter firmware behavior,
flash a gateway, or connect to a production cloud service.

## Matrix and evidence boundary

| Scenario | Actual executed path | Result / boundary |
| --- | --- | --- |
| Mixed old/new devices online | Three localhost TCP MQTT clients → production HubTransport/HubBroker → actual hub.inc discovery/Home/Relay event dispatch | PASS: old JSON TC1, MP A1, MP M1 concurrently connected |
| Discovery format selection | Real retained discovery publishes; schema 1 JSON and canonical schema 2 MP advertisements | PASS: independent device codecs; invalid identity cannot downgrade known MP peer |
| Controller outlet and fan writes | HubHome.begin → broker → real TCP device command → receipt → status.get → new device state | PASS: old JSON outlet.set and MP fan.set; pre-receipt state cannot complete write |
| Wrong device format | JSON state sent to discovered MP A1 through socket/broker | PASS: prior sequence unchanged and pending control remains incomplete; no codec fallback |
| Sensor mapping | MP M1 state through socket/broker → HubHome model | PASS: temperature 25.5 reaches model; fixed model mapping, not dynamic sensor-field discovery |
| Bemfa conversion | Incoming textual command → actual HubRelay → broker/socket → old JSON or new MP command → receipt | PASS: both profiles; retained commands rejected; normal echo suppression respected |
| Cloud bridge | Device socket → broker/Relay upstream callback; incoming callback → broker → subscribed socket | PASS: embedded NUL bytes preserved both directions; upstream-injected state cannot become local device confirmation |
| Offline recovery | Device availability/state publishes → actual broker/Home callback | PASS: offline disables control; explicit online plus valid MP state restores reachability |
| LAN pairing and revocation | MiCO production encoder → actual DAIKIN pollSessions; shared actual receiver and DAIKIN sender in both directions | PASS: empty/password pair, owned async credentials, durable resume/reboot, key-vs-token, lost ACK retry, unpair, recovery gate |
| UDP legacy JSON rejection | JSON read/auth_pair supplied to actual pollSessions | PASS: no reply; UDP has no JSON compatibility path |
| Remote display producer | Actual publishState → local broker and SDK output boundary | PASS: retained schema-3 metadata precedes nonretained complete values; matching boot/descriptor revision; field units only in metadata; repeated 40 publications |
| Persistence and queued auth | Actual production persistence and full main webPending dispatch | Existing PASS rerun: unknown durable failure rejects captured reset/reboot/query/status work; migration preserves both key tables |

`test_hub_mixed_devices.py` compiles the real business classes with ASan/UBSan.
Only simulated devices and external controller/cloud/Bemfa I/O are supplied by
the fixture. The hub event dispatch statements are extracted from production
`hub.inc`, not rewritten as a test router. MQTT commands and replies traverse
real localhost sockets and the embedded transport/broker implementation.

The full Python suite passed 113 tests with two optional skips (official NVS
source absent in that invocation; SDK worker integration not enabled). The
final mixed-device test also passed after adding incoming cloud binary delivery
and upstream-state isolation. Legacy Home/Relay/Service/HTTP standalone fixtures
are executed separately with the pinned ArduinoJson and ASan/UBSan.

## Compatibility scope and gaps

JSON conversion remains permitted at the hub's old-device MQTT adapter and
external JSON/text integrations, including HA and Bemfa. New device management
HTTP/MQTT uses MP; raw bridge mappings forward bytes as configured and do not
retry writes using another codec. UDP is MP-only, including pairing and sensor
reads. Internal ArduinoJson documents are still used by DAIKIN business logic.

This acceptance does not establish physical HomeKit pairing, physical gateway
NVS layout, radio behavior, MCU scheduler latency, or internet service behavior.
The remote display test proves the DAIKIN producer contract; final display
consumer rendering, stale/boot/revision rejection, and physical sensor-to-display
operation require the display/MCU lane and DUT results. HubHome currently maps
fixed per-model metrics; dynamic schema-4 sensor_fields-driven HAP service
creation is not implemented or claimed by this matrix.

## Reproduction

After installing the pinned PlatformIO DAIKIN dependencies and Python build
requirements:

```sh
python3 -m unittest DAIKIN.tests.test_hub_mixed_devices \
  DAIKIN.tests.test_lan_pairing DAIKIN.tests.test_auth_storage \
  DAIKIN.tests.test_native_publish -v
python3 -m unittest discover -s DAIKIN/tests -p 'test_*.py' -v
```

Local logs in the surrounding campaign workspace:

- `artifacts/daikin/hub-simulation-all-tests.log`
- `artifacts/daikin/hub-simulation-mixed-final.log`
- `artifacts/daikin/hub-simulation-legacy-models.log`

No physical acceptance or production deployment is inferred from these logs.
