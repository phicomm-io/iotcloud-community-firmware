# Device names

`name.set` changes the persistent shared device name. HTTP and MQTT use the
existing command envelope; authenticated UDP requires CONTROL scope. SENSOR
scope cannot rename a device. General `config.set` remains unavailable over UDP.

The MessagePack command map contains `action: "name.set"`, a unique
`request_id`, and `params: {name: "…"}`. `params` permits only `name`, which must
be a nonempty valid UTF-8 string without NUL bytes. `capabilities.get` advertises
`name.set` in `actions` and the byte limit in `name_max_bytes`: TC1 accepts up to
31 bytes; A1, M1, DAIKIN and S30-V1B accept up to 63 bytes. The existing transport
authentication and write admission also apply to this command.

Successful persistence returns `accepted`. Invalid names return
`invalid_params`; storage failures return `storage_error`; admission failures
return `busy`. Read `status.get.name` after the correlated receipt to confirm
the saved value. A rename preserves other configuration and the stable device ID.
An uncertain write outcome requires reading state before deciding on another
explicit command; controllers must not retry mutations automatically.

The DAIKIN HomeKit bridge exposes the confirmed name as Name and ConfiguredName.
The first Switch service of TC1/S30, the A1 Fan service, or the M1/DAIKIN
TemperatureSensor service owns ConfiguredName. Writes require current peer
capabilities and complete only after the `name.set` receipt and a subsequent
matching status snapshot. Failures keep the previous exposed name. Existing
accessory and characteristic IDs remain unchanged; ConfiguredName uses IID 1000.

Controllers that write ConfiguredName can change the shared device name.
Apple Home ordinary accessory aliases are stored by the controller and do not
send a name update to the device. Edit the name in device settings when a shared
name is required across controllers.
