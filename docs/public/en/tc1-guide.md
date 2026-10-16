# TC1 operation

TC1 provides six outlet controls, metering, child lock, an indicator and schedules. For first installation, see [TC1 flashing](tc1-flash).

## Outlets and home page

The home page shows all six outlets and a master switch. Control each outlet separately or switch all six together. Each outlet can have a custom name saved on the device. Child lock blocks custom physical-button actions.

## Metering

Read total instantaneous power, accumulated energy, peak values, activation counts and the power trend chart. The metering report interval is configurable. Readings use a device-side calibration factor; compare a stable load with an independent meter before changing it to correct a consistent offset.

## Indicator

Connection warnings override normal indicator settings. Normal lighting returns when connectivity is healthy. The indicator may stay on or be disabled for ordinary operation; see [connection indicators](faq#connection-indicators).

## Schedules, time and time zone

Configure NTP and the device time zone, confirm time synchronization, then enable schedules. Tasks can repeat weekly or run on a date and target individual outlets. The number of stored tasks is bounded and the list is loaded in batches. Night periods whose end is earlier than their start cross midnight.

## Buttons

Custom mappings support 1–30 clicks (executed after the click interval) and long presses of 1–4 seconds. Five seconds and above are reserved. **TC1 has no default custom button action**; configure one explicitly. Child lock blocks custom actions. View, edit or delete mappings in settings; identical consecutive mappings are folded in the UI.

### Recovery gestures

The main button performs recovery **when released**:

| Hold duration | Action |
| --- | --- |
| 5 to less than 10 seconds | Reset network |
| At least 10 seconds | Factory reset |

A short light pulse marks each full second held without changing saved lighting settings. Pulses only indicate elapsed time; they do not execute recovery early. Recovery bypasses child lock, while ordinary actions obey it.

Make sure you can provision Wi-Fi again. Factory reset clears user settings; **a full TC1 reset leaves all six outlets on by default**.

## MQTT

TC1 supports Legacy (v1) and TMCP (v2), defaulting to TMCP for a new setup. See [MQTT and automation](mqtt-link) for Home Assistant and platform configuration.
