# MQTT subscription refusal classification

The RC4 M1 adapter needs to distinguish a refused optional subscription from a
broken MQTT exchange. The final overlay returns `MQTT_SUBSCRIBE_DENIED` (-4) only
after receiving an exact SUBACK header/length, one result, a matching packet ID,
and result byte 128. Other errors retain their existing failure behavior.

The SDK deserializer returns a plain `char`, so the result comparison normalizes
its signedness. The fixture covers native signed char and ARM-style unsigned
char, with both short and default enum layouts, under ASan/UBSan.

Validation on 2026-10-28: seven tests passed against pinned SDK
`6c465211d3ff8797cd835e400ec54a06530dd476`:

```
python3 -m unittest -v tests.test_mqtt_subscribe_qos tests.test_mqtt_overlay_preparation
```

The fixtures check QoS0/1/2, denial, wrong IDs (including a denied wrong ID), bad
headers, short/long replies, excess QoS, read/write failure, and timeout. The old
short-enum implementation remains a failing ASan negative control. All three
product overlay stacks upgrade from both previous complete prefixes and remain
idempotent; an incompatible partial stack remains unchanged.

The [test log](evidence/rc4-mqtt-subscribe-result.log) records host protocol and
overlay evidence only. It does not establish target builds or device acceptance.
