# A1 operation

A1 provides fan control, a light ring, child lock, night mode and automatic control using M1 readings. For installation, see [A1 / M1 flashing](a1-m1-flash).

## Power and fan

The home page provides independent power and operating-mode controls.

- Manual mode runs at your chosen speed. Returning to manual restores the last manual speed.
- Auto mode adjusts speed while powered on, using PM2.5, formaldehyde, or the higher demand of both.
- Selecting a mode while off preserves power-off. Worsening air quality never turns on a powered-off purifier.
- The ring and web slider set speed in manual mode and the active period's maximum in auto mode. Changing speed also turns power on.

Optional **start/idle hysteresis** adds a target and a higher start threshold. For a PM2.5 target of 35 and start threshold of 50, the motor starts at 50, idles at 35, and retains its state between them. Powering on inside this band initially idles. Any selected pollutant can trigger start; all must meet their targets to idle. Conditions must remain true for the configured duration, and transitions respect minimum run/idle time.

While running, the chosen fixed, linear or PID algorithm adjusts speed; PID shares the same target. Running speed is at least 1%, while idle is 0% regardless of the minimum speed. Night limits still apply. Changing a speed cap or night profile preserves the latch. Disabling hysteresis restores continuous speed control. Both the local WebUI and the MQTT remote management platform display mode and running/idle state and can edit these settings.

Configure minimum, maximum and sensor-offline speeds. A minimum of **0%** allows the motor to idle while logical power stays on; worsening air quality resumes operation after the debounce and minimum switching intervals. Fixed, linear and PID strategies remain available. Missing, unbound or stale sensor data uses the offline speed clamped to the current range; fresh data restores automatic control. Offline fallback never turns on a powered-off purifier.

Each night window independently overrides minimum, maximum and offline speeds while sharing the daytime algorithm. The last matching fan window wins in auto mode; manual speed remains capped by night limits. Before clock synchronization the daytime settings apply.

The management platform’s **Automatic speed settings** button and automatic-control card heading both open the settings. A loading message appears while reading; failed reads can be retried. The A1 device in Home Assistant has an independent **运行模式** (operating mode) selector: **自动** (auto) and **手动** (manual) preserve the power state. Fan details also provide preset modes. HomeKit Bridge synchronizes the fan’s auto/manual presets. If Apple Home still shows old controls after upgrading, reload HomeKit Bridge in HA and reopen Apple Home.

## Estimated filter life

The home page shows remaining life and accumulated fan running hours. Standby and stopped time do not count; manual, scheduled, night and automatic operation all count equally, regardless of speed. The default estimate is **4320 hours** (180 days continuously). Expand “Adjust rated lifetime” to set 1–20000 hours according to your filter specification; changing this value preserves accumulated usage. Filter condition and air quality also affect when replacement is needed.

At 10% or less, the page recommends replacement; at exhaustion it stays at 0% and fan controls remain available. After installing a new filter, select “Reset after replacing filter” and confirm. This clears usage and restores 100%, preserving the configured lifetime.

Usage is saved every hour of operation, when the fan stops, and before a normal restart or update. Sudden power loss may lose the most recent partial hour. A network reset preserves usage; a factory reset clears it and restores the default lifetime. The first upgrade starts at zero because previous filter usage is unknown.

## Light ring

Successful mode selections show a brief confirmation: **two cyan pulses for auto, one warm amber pulse for manual**. The ring returns to its normal effect after about 1.2 seconds. Web, button and Home Assistant selections all receive feedback, including selecting the current mode again. Confirmation can briefly light the ring with power or normal lighting off, without starting the fan or changing saved lighting settings. The night brightness limit still applies.

The ring shows air quality, connection progress and operation feedback. Configure air-quality colors, segment ranges and the colors corresponding to reading ranges. Connection warnings override normal lighting after normal effects are rendered and remain subject to the night brightness limit. Normal lighting regains control when connectivity is healthy. See [connection indicators](faq#connection-indicators).

## Child lock and night mode

Child lock blocks normal physical-button actions; a configured child-lock gesture can still unlock the device. Night mode reduces brightness during the configured time period. An end time earlier than the start means the period crosses midnight.

## M1 automation

All automation configuration lives on A1; M1 only measures and responds.

1. Add authorized M1 members.
2. Configure automatic speed range, offline speed, algorithm, debounce and minimum switching intervals.
3. Select auto mode and turn on power. Without valid data the offline speed applies.

A1 computes the arithmetic mean of bound M1 readings no older than 15 seconds. With no valid data, readings are `null`. A1 initiates sessions, probes and reads, preferring LAN and falling back to MQTT. It periodically reinitializes sessions after M1 restarts without manual token entry. **Upgrade both sides together.** See [pairing instructions](mqtt-link#a1-and-m1-automation).

## Buttons

A1 has a power button and a Wi-Fi button. The default power-button actions are:

| Gesture | Action |
| --- | --- |
| Single click while off | Power on, keeping the selected auto/manual mode |
| Single click while on | Show the current mode and airflow, keeping power on |
| Double click | Toggle auto/manual mode, keeping the power state |
| Hold for 1–4 seconds, then release | Power off |

When showing status, the ring first confirms the mode with **two cyan pulses for auto or one warm amber pulse for manual**. After about 1.2 seconds, a speed bar in the same color shows actual airflow for about 3 seconds. Auto idle has zero airflow, so the bar is dark after the mode confirmation. The bar shows actual airflow rather than the automatic ceiling or remembered manual speed. The normal effect resumes afterward. Feedback can briefly wake disabled normal lighting and still obeys the night brightness limit. A power-button press while running no longer adds a green key flash; the green bar is kept only when turning on from off. Inspection leaves mode, speed and lighting settings unchanged and is blocked by child lock.

Upgrades replace the previous default single-click power toggle with “Power on / show current mode and airflow” and assign power-off to empty power-button hold slots. Existing custom single, double, hold and combination actions are preserved and can be reassigned in button settings.

 In **Settings → Automation → Button events**, select power, Wi-Fi or both buttons, then choose a gesture and action. A combination requires both buttons to overlap; it cancels the single-button gestures for that interaction. Releasing the buttons at different times does not trigger Wi-Fi recovery.

Custom mappings support 1–30 clicks, executed after the click interval, and 1–4 second long presses, executed on release. Five seconds and above are reserved. Actions include cycling or selecting a light effect, resetting filter usage, fan power and speed, ring power and brightness, always-on lighting, child lock and reboot. Switch actions support on, off and toggle; ring toggles can preserve brightness. Identical consecutive mappings are folded in the UI.

### Filter life

The button-events page also shows filter usage, allows rated-lifetime edits and resets usage after replacement. Reset can be bound to a gesture. The estimate follows the filter-life rules above.

### Recovery gestures

The Wi-Fi button performs recovery **on release**:

| Hold duration | Action |
| --- | --- |
| 5 to less than 10 seconds | Reset network |
| At least 10 seconds | Factory reset |

A short pulse marks each full second held without changing saved lighting settings. It only indicates time; recovery never executes early. Recovery bypasses child lock. Ensure you can provision Wi-Fi again because factory reset clears user settings. A full reset is also available in the WebUI.

## MQTT

A1 uses TMCP (v2) topics only, with no Legacy selector. See [MQTT and automation](mqtt-link).


## Home Assistant and HomeKit modes

The HA purifier entity exposes `auto` and `manual` presets. Web, physical buttons and HA report the same mode, including while power is off. HA speed changes select manual and set its speed; the web and ring adjust the cap while in auto.

If HomeKit still shows a generic fan, set A1's type in the existing HomeKit Bridge's `entity_config`. Replace the entity IDs with the actual HA entities:

```yaml
entity_config:
  fan.your_a1:
    type: air_purifier
    linked_pm25_sensor: sensor.your_a1_pm25
```

The PM2.5 link is optional and can be omitted without a bound M1. See the [Home Assistant HomeKit Bridge documentation](https://www.home-assistant.io/integrations/homekit/).

When exposed as an air purifier through HomeKit Bridge, automatic maps to HA's `auto` preset. The current bridge sends a speed command when manual is selected, so it applies HomeKit's displayed speed. That speed command can also turn on a powered-off device. To restore the last manual speed while preserving power state, select manual directly in HA or the web page. The bridge can display zero-speed idle as purifying or minimum speed; use the device page to confirm actual idle state.

## Optional CO₂ linkage

When a linked M1 has an SCD40, A1 shows mean CO₂ in ppm and offers CO₂ alone or combined with PM2.5 and formaldehyde for automatic speed control. Upgrade both A1 and M1 to firmware supporting CO₂ linkage. M1 units without a CO₂ sensor do not contribute to the CO₂ average. CO₂ has an independent freshness deadline: when selected and all CO₂ readings are missing or stale, A1 uses the configured offline speed rather than treating missing readings as zero. PM2.5/formaldehyde-only control continues independently.

Default CO₂ thresholds are 1500 ppm to start and 1000 ppm to idle; the default linear range is 800–2000 ppm. These values are adjustable. Combined control uses the highest speed demand. With start/idle hysteresis, any selected metric may start the fan, while all selected metrics must reach their targets before idling.
