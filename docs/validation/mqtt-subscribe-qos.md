# MQTTSubscribe short-enum storage correction

The pinned SDK calls `MQTTSerialize_subscribe(..., (int *)&qos)`, but its
`enum QoS` parameter occupies one byte on both actual ARM GCC5.4.1 and
GCC14.3.1 under the product flags. The serializer reads `requestedQoSs[i]`
as a four-byte int. This violates object bounds and effective-type rules;
GCC14 reports it, whereas GCC5 does not diagnose it. No extra
`-fshort-enums` flag is needed for those ARM compilers. The host regression
uses that flag to reproduce the target ABI.

`common/mqtt-subscribe-qos.patch` creates an ordinary int initialized from
validated QoS, and passes its address. It does not change public layouts,
QoS validation, SUBACK validation, retained state or handler-slot policy.
The overlay is the final member of the ordered MQTT stack in all three
product builders, host/TC1/simulator builders and shared SDK fixtures.
The preparation regression verifies clean application, previous six-overlay
SDK upgrade, repeated application and rejection of incompatible partial
preparation without mutation. The native unified runner and CI register the
actual SDK sanitizer regression, whose negative control explicitly omits
only this final overlay.
The current main/97de7f7 images and compiler comparison remain unchanged.

The actual SDK MQTTClient and packet serializers, compiled with ASan/UBSan,
reproduce stack-buffer-overflow before the fix. Fixed short/default-enum
fixtures each test QoS0/1/2 100 times, exact outgoing QoS bytes, denied and
excessive grants, wrong IDs, malformed SUBACK and handler reuse.
Run `PHICOMM_TC1_SDK=<pinned SDK> python3 -m unittest
 tests.test_mqtt_subscribe_qos -v`.

This is a real bug, but it does not alone prove the M1 waiting_acl cause:
on little-endian ARM, the serializer ultimately writes only the low byte
of the over-read int. A successful subscription may therefore appear to
work despite undefined behavior. Any live candidate must verify actual
SUBACK/result and MQTT readiness; offline tests are not that evidence.
The warning blocks accepting GCC14 as a general release toolchain until
corrected/reviewed, rather than being suppressed. No DUT was accessed.

Integration validation (no DUT writes): all three product-specific pinned
SDK variants upgrade from the previous six-patch stack to the same files as
a clean seven-patch application; a second application is byte-identical.
The six build entry points are checked against the exact ordered stack.
Eight overlay/storage/QoS tests and 26 shared MQTT tests pass under the
actual pinned SDK. Twelve target TU builds (three products, GCC5/GCC14,
old/fixed) pass with original per-product compiler options. The fixed GCC14
TU no longer reports the invalid enum/int access. `MQTTSubscribe` remains
96 bytes in every emitted `.su` file, so the existing SDK stack reserve and
application stack gates are unchanged. This is target compilation evidence,
not a claim that all three complete firmware images were rebuilt here.
Build source timestamps/dependency files trigger recompilation of the
patched SDK TU; no public headers or structures change.
