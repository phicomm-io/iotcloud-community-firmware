# M1 operation

M1 provides PM2.5, formaldehyde, temperature, humidity and screen brightness controls. It can supply readings for A1 automation. For first installation, see [A1 / M1 flashing](a1-m1-flash).

## Readings

The home page shows particle concentration (PM2.5), formaldehyde concentration, ambient temperature and relative humidity. Colors and labels indicate reading quality, such as excellent, good or poor.

## Screen brightness

Set brightness in the WebUI; the device saves and acknowledges it. MQTT commands can also change brightness, and the device reports the accepted setting when an independent MQTT client changes it.

## Wi-Fi and error icons

The original panel has two independent icon outputs:

| State | Wi-Fi icon | Error icon |
| --- | --- | --- |
| Normal | Steady on | Off |
| Wi-Fi fault | Blinking | Blinking |
| Configured MQTT fault | Steady on | Two long and two short pulses |

Disabling an output drives its GPIO low. An unconfigured MQTT connection is not a fault. The original screen's formaldehyde / PM2.5 color thresholds and manual color controls have no verified protocol and are not supported.

## Buttons and recovery

M1 uses the original panel's `0x18` long-press event, acknowledges it with fixed `0x16`, and clears Wi-Fi and the WebUI password once per boot after deduplication. It does not implement TC1 / A1's precise 5 / 10 second recovery split and has no custom button mappings. Follow [A1 / M1 recovery](a1-m1-flash). A full reset is available in the WebUI.

## Supplying readings to A1

M1 measures, reports and answers authorized A1 requests. It does not control or poll the purifier. Add `A1_MAC:M1_MAC` in M1 binding settings; at most eight unique pairs are accepted. Configuration reads / writes, unbinding and sensor pushes are rejected on the binding channel. See [A1 and M1 automation](mqtt-link#a1-and-m1-automation).

## MQTT

M1 uses TMCP (v2) topics only, with no Legacy selector. See [MQTT and automation](mqtt-link).
