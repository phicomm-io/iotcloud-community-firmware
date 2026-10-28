# Troubleshooting

This page covers community firmware. For A1 / M1 still running factory firmware, see [stock Wi-Fi setup](stock-provision).

## Connection indicators

Connection warnings take priority over normal lighting:

**Wi-Fi not configured > Wi-Fi disconnected > configured MQTT not ready > normal.**

| State | Indication |
| --- | --- |
| Wi-Fi not configured | Three long pulses (800 ms), then three short pulses (200 ms), with 200 ms between pulses and a 2000 ms pause after the last pulse; repeats |
| Wi-Fi disconnected | Approximately 150 ms on / 150 ms off |
| Configured MQTT not ready | Approximately 1 second on / 1 second off |
| Normal | Normal lighting settings regain control |

TC1 uses its indicator and restores normal settings when connected. M1 uses separate Wi-Fi and error icons: normally Wi-Fi is on and error is off. A1 uses a connection-indicator position in the ring; the remaining lighting follows its settings. DAIKIN uses its separate Wi-Fi indicator.

**An unconfigured MQTT connection is not a fault.** MQTT warnings appear only when MQTT was configured but is not ready.

## Common problems

### Fast blinking / cannot connect

Wi-Fi is disconnected. Check the router, password and signal range. Reset network settings and provision Wi-Fi again if necessary.

### Online device with slow blinking

Check the configured Broker address, port, credentials and reachability. If MQTT has not been configured, there should be no MQTT fault warning.

### Cannot find the IP address

Find the device MAC in the router's DHCP client list. The setup hotspot opens only when Wi-Fi is unconfigured, not while the device is connected.

### Forgotten WebUI password

TC1's main button and A1's Wi-Fi button reserve recovery gestures, performed **on release**: hold for 5 to less than 10 seconds to reset the network, or at least 10 seconds for a factory reset. Ensure you can provision Wi-Fi again; factory reset clears user settings.

On M1 with the stock sensor controller, **hold the body button for 9 seconds or longer, then release**. This clears Wi-Fi and the WebUI password and reboots, preserving other user settings.

### Buttons seem unresponsive

Disable child lock for custom actions. TC1 / A1 custom actions and recovery execute after release. Five seconds and above are reserved, not custom long presses. Multi-click actions wait until the click interval finishes.

### A1 sensor automation does not work

Check the provider MAC and optional IP on A1; M1 and DAIKIN need no manual pairing identifier. Check members and enabled rules, updated firmware, the same MQTT service/account and generic sensor ACLs. Check LAN UDP and MQTT measurements. Missing metrics hide their choices; no valid required data means `null` and the configured offline speed.

### Update failed

Keep power connected during updates. If the WebUI remains available, verify the file and network and retry. Otherwise recover with a programmer: [TC1](tc1-flash) [A1 / M1](a1-m1-flash) or [DAIKIN](daikin-guide).

### Settings were not saved

Network changes may disconnect the browser; reconnect and check the settings. Also check for a power interruption while saving.

### An account or device is missing after changing nodes

China and US are independent platforms. Return to the original node to use its account and devices, or register separately on the new node. Devices must connect to that node's Broker. Node switching does not migrate data.

## Factory reset

A factory reset clears user settings and requires Wi-Fi provisioning again. A full TC1 reset leaves all six outlets on by default. Before resetting, make sure you know the Wi-Fi password and the device is within signal range.

## More help

Read the [TC1](tc1-guide), [M1](m1-guide) [A1](a1-guide) or [DAIKIN](daikin-guide) guide. Search or open a [firmware issue](https://github.com/phicomm-io/.github/issues), including the product, firmware version and reproduction steps.

Join the Chinese-speaking QQ community: **1021750178** · [Join the group](https://qm.qq.com/q/9L150V85kk?from=tim). If the link does not open, search for the group number in QQ.

Include the product, firmware version, whether you use the local WebUI or MQTT / Home Assistant, reproduction steps and screenshots. Remove passwords and tokens before sharing.
