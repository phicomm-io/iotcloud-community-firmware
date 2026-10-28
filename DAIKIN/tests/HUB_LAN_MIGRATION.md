# Hub LAN transport test boundary

The product is migrating from an embedded MQTT broker to mDNS discovery and
explicitly authenticated UDP. Application route keys remain for cloud and Bemfa
mappings; they do not identify local MQTT subscriptions.

The following execute production business models through a synchronous
`HubDeviceBus`, with ASAN/UBSAN:

- `test_hub_business`: local owner configuration/isolation, HomeKit model
  ACK followed by measured-state confirmation, memory/retry limits, cloud routes
  and Bemfa responses.
- `test_hub_mixed_devices`: legacy JSON/new MessagePack codec selection, HAP
  model control and state, wrong-codec rejection, Bemfa, binary cloud forwarding,
  injected-state isolation, offline/recovery. Discovery objects here are cloud
  protocol fixtures, not actual mDNS advertisements or a UDP pairing test.
- `test_hub_device_bus`: fail closed without a transport handler, payload/route
  limits, binary NUL preservation and synchronous borrowing contract.

Historical embedded-MQTT suites are explicitly skipped, not counted as passed:
`test_hub_mqtt`, `test_hub_admission`, `test_hub_byte_memory`,
`test_hub_topic_budget`, `test_hub_transport_will`.
Their C++ fixtures are retained for traceability, not product support.

`test_hub_reload` and `test_hub_restore` extract old embedded transport lifecycle
code and rely on TCP client/retained-store assertions. They are also explicitly
skipped during migration. This leaves a real regression gap: authenticated UDP
peer revocation, in-flight request cancellation, reboot/restore/reload and
upstream client shutdown/reconnect must be revalidated against the new runtime.
No UDP lifecycle coverage is claimed by the bus tests.

`test_hub_config_storage` remains independent and unchanged. Its optional IDF/NVS
source dependency may cause an environmental skip.

Run new business tests after fetching the pinned ArduinoJson dependency, or set
`DAIKIN_ARDUINOJSON_SOURCE` to that dependency's `src` directory:

```sh
python3 -m unittest DAIKIN.tests.test_hub_business \
  DAIKIN.tests.test_hub_device_bus DAIKIN.tests.test_hub_mixed_devices
```
