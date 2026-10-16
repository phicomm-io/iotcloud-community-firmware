# A1 operation

A1 provides fan control, a light ring, child lock, night mode and automatic control using M1 readings. For installation, see [A1 / M1 flashing](a1-m1-flash).

## Power and fan

The home page has power and fan-speed controls. Turn the purifier on or off, select a manual speed, or use automatic control based on M1 readings and configured rules.

## Light ring

The ring shows air quality, connection progress and operation feedback. Configure air-quality colors, segment ranges and the colors corresponding to reading ranges. Connection warnings override normal lighting after normal effects are rendered and remain subject to the night brightness limit. Normal lighting regains control when connectivity is healthy. See [connection indicators](faq#connection-indicators).

## Child lock and night mode

Child lock blocks custom physical-button actions. Night mode reduces brightness during the configured time period. An end time earlier than the start means the period crosses midnight.

## M1 automation

All automation configuration lives on A1; M1 only measures and responds.

1. Add authorized M1 members.
2. Configure enable switches, thresholds, fan policy, debounce and minimum switching intervals.
3. Confirm valid average readings before enabling automation.

A1 computes the arithmetic mean of bound M1 readings no older than 15 seconds. With no valid data, readings are `null`. A1 initiates sessions, probes and reads, preferring LAN and falling back to MQTT. It periodically reinitializes sessions after M1 restarts without manual token entry. **Upgrade both sides together.** See [pairing instructions](mqtt-link#a1-and-m1-automation).

## Buttons

A1 has a function button and a Wi-Fi button. By default, a single function-button click cycles the fan speed. Custom mappings support 1–30 clicks, executed after the click interval, and 1–4 second long presses. Five seconds and above are reserved. Child lock blocks custom actions; identical consecutive mappings are folded in the UI.

### Recovery gestures

The Wi-Fi button performs recovery **on release**:

| Hold duration | Action |
| --- | --- |
| 5 to less than 10 seconds | Reset network |
| At least 10 seconds | Factory reset |

A short pulse marks each full second held without changing saved lighting settings. It only indicates time; recovery never executes early. Recovery bypasses child lock. Ensure you can provision Wi-Fi again because factory reset clears user settings. A full reset is also available in the WebUI.

## MQTT

A1 uses TMCP (v2) topics only, with no Legacy selector. See [MQTT and automation](mqtt-link).
