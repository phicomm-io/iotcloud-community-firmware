# DAIKIN Hub HomeKit adapter

The HAP adapter exposes only devices registered to the local hub owner. TC1
uses six Switch services named Outlet 1 through Outlet 6. Its relay state
does not prove that an appliance is physically plugged in; therefore the
adapter does not expose an OutletInUse characteristic. Add Outlet services
only if reliable per-outlet insertion sensing becomes available. Apple's
[HomeKit ADK characteristic definition](https://github.com/apple/HomeKitADK/blob/master/HAP/HAPCharacteristicTypes.h#L515-L524)
defines OutletInUse independently of the relay's on/off state.

A1 fresh-air variants are recognized from `fresh_air_on` and
`fresh_air_speed`. Those fields describe applied output; the adapter uses
`fresh_air.set` and does not infer output from requested settings or the
primary fan. Ordinary A1 models continue to use `fan.set`.

HomeKit power and level changes in one service update produce one TMCP
mutation. Explicit off takes priority over a positive level. `light.set`
accepts brightness as 0–100 percent; state reports brightness as 0–255.
Successful controls require a matching successful receipt followed by a
fresh measured snapshot. A status query after the receipt supplies a
confirmation even when no periodic state publication follows promptly.

Pairing storage must remain healthy throughout initialization and runtime.
The HAP server starts only after all services are built and HomeSpan has
successfully initialized its store. Storage failure closes HAP sessions and
disables control until recovery. Pairing reset stops HAP before attempting
erase because a failed erase may already have modified storage.
Factory reset also erases recognized pairing storage when its NVS is corrupt.
If the reserved tail is unidentified, ordinary firmware settings can reset
while those unknown tail bytes remain untouched.
On 4 MiB Flash, the store accepts only the verified stock 1.5 MiB or wired
1.875 MiB dual-slot layouts and retains their installed tables, using the
`0x3d0000..0x400000` tail only after its blank/ownership checks pass.

Host tests cover command units, variant detection, applied-state
confirmation, off precedence, owner sessions, configuration validation and
failed-save rollback. Apple Home pairing, client UI and physical device
acceptance require separate runtime validation.
