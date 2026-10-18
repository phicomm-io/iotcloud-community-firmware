# M1 operation

M1 provides PM2.5, formaldehyde, temperature, humidity and screen brightness controls. It can supply readings for A1 automation. For first installation, see [A1 / M1 flashing](a1-m1-flash).

## Readings

The home page shows particle concentration (PM2.5), formaldehyde concentration, ambient temperature and relative humidity. Colors and labels indicate reading quality, such as excellent, good or poor.

## External SCD40 CO₂ sensor (experimental)

Test firmware with SCD40 support reads CO₂ over I²C. After sensor identification, the home page displays CO₂ in ppm. A newly opened page hides the card until a valid reading arrives, including when the sensor data is already expired. If a reading expires after a valid value was displayed, the card retains the last valid value from this page session in gray with an “Outdated” label. Before the first valid reading, it shows “—”. Reloading clears this cache. Original temperature and humidity readings still come from M1's built-in sensor.

Hardware has produced valid readings, but continuous sampling has also stopped during testing. Stability validation is incomplete; this experimental feature does not imply support in published firmware.

Disconnect power and identify solder points on the actual board. These are SDK GPIO names, not connector pin numbers:

![M1 external SCD40 wiring photo](assets/images/m1-scd40-wiring.png)

The handwritten **7 / 8 mean GPIO7 / GPIO8**, corresponding to SCL / SDA below, not module physical pin numbers. GND / VCC arrows locate the supply pads; confirm voltage and polarity on your board before connecting.

| M1 signal | SCD40 module |
| --- | --- |
| GPIO8 (I²C1 SDA) | SDA |
| GPIO7 (I²C1 SCL) | SCL |
| Stable supply matching the module rating | VCC / VIN |
| GND | GND |

Pull SDA and SCL up separately to **3.3 V**. Start with 4.7 kΩ for short wires; existing 10 kΩ pull-ups to 3.3 V can be tried first. Check the pull-up supply; do not pull M1's bus to 5 V. Keep wires short and provide a supply that handles measurement current peaks, never GPIO power. GPIO9 / GPIO10 are used by the original UART.

References: [Sensirion SCD40](https://sensirion.com/products/catalog/SCD40) and [official developer and power guidance](https://developer.sensirion.com/product-support/scd4x-co2-sensor).

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

## IotCloud and Home Assistant

IotCloud reuses the M1 CO₂ detail card and shows fresh CO₂ readings in device summaries. Once SCD40 produces a valid reading, Home Assistant discovers a carbon dioxide MQTT sensor in ppm. After 20 seconds without an update, only that sensor becomes unavailable; the original M1 sensors remain independent.

## MQTT

M1 uses TMCP (v2) topics only, with no Legacy selector. See [MQTT and automation](mqtt-link).
