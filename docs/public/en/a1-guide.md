# A1 operation

A1 provides fan control, a light ring, child lock, night mode and automatic control using valid readings from M1, DAIKIN and other sensor providers. For first installation, see [A1 factory OTA](a1-no-disassembly) or [A1 wired recovery](a1-flash).

**Use stable v0.9.27 for everyday operation, or v1.0.0-rc.5 for beta testing.** See the [complete candidate release notes](release-log.html?channel=candidate) for version requirements and cumulative changes.

## Power and fan

The home page provides independent power and operating-mode controls.

- Manual mode runs at your chosen speed. Returning to manual restores the last manual speed.
- Auto mode adjusts speed while powered on, using PM2.5, formaldehyde and CO₂; combined metrics use the highest demand.
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

Breathing and rainbow effects have an animation speed control under Settings → Light effects. Levels 1–10 run from slow to fast; level 5 preserves the original speed. Save to keep the setting after a restart.

## Shutdown countdown

Open Settings → Schedules, choose Shutdown countdown, enter hours and minutes, and save. Durations range from 1 minute to 24 hours. The purifier turns off when time is up in either manual or auto mode. The list shows remaining time and lets you cancel or restart from the full configured duration.

Countdowns work without clock sync and continue after closing the browser. A power loss or restart cancels them. They share the eight schedule slots with normal tasks. If the device is busy when due, the task is marked as skipped and is not repeated. Weekly and dated tasks still require clock sync.

## Child lock and night mode

Child lock blocks normal physical-button actions; a configured child-lock gesture can still unlock the device. Night mode reduces brightness during the configured time period. An end time earlier than the start means the period crosses midnight.

## Sensor automation

Automation choices depend on currently valid measurements, independently of the monitor model. Add up to four providers, including a mix of M1 and DAIKIN. PM2.5, formaldehyde and CO₂ are checked independently for units, validity and freshness before aggregation. Missing or stale metrics hide their options while preserving saved rules. Unavailable required data triggers the configured offline speed; missing values never become zero. PM10 and TVOC currently remain monitor readings rather than A1 control metrics.

CO₂ defaults are 1500 ppm to start, 1000 ppm to idle and an 800–2000 ppm linear range. Any selected metric can trigger start; all must meet their targets to idle. These thresholds remain configurable.

On A1, add each provider MAC and optional IP. Configure automatic speed range, offline speed, algorithm, debounce and minimum switching intervals, then select auto mode and turn on power. No manual A1 pairing identifier is needed on the monitor.

A1 authorizes temporary sessions through the same MQTT account, prefers LAN UDP reads and can use generic MQTT measurements when LAN reads are unavailable. Existing M1 sessions remain compatible. PM2.5, formaldehyde, temperature and humidity expire after 15 seconds; CO₂ expires after 20 seconds. Replayed samples do not renew freshness. Each average includes only providers with valid data for that metric; absent data is `null`.

Use updated firmware on both sides and Broker permissions for generic sensor topics. See [MQTT and automation](mqtt-link).

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

Some original touch panels report release after about 2–3 seconds of continuous contact, preventing the longer holds above. **v1.0.0-rc.3 adds** these alternatives (unavailable in rc.2 and earlier):

| Wi-Fi button gesture | Action |
| --- | --- |
| Two short taps, then hold the third touch for 1–2 seconds and release | Reset network |
| Three short taps, then hold the fourth touch for 1–2 seconds and release | Factory reset |

Release after each short tap and leave less than 0.3 seconds between touches. The final hold must last at least 1 second and less than 5 seconds. These reserved recovery sequences execute when the panel reports release, bypass child lock, and consume the preceding taps and final hold without running their custom actions. Separate click groups, standalone holds and two-button combinations remain available. These alternative sequences remain experimental. Factory reset clears user settings, so ensure you can provision Wi-Fi again.

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

The PM2.5 link is optional and can be omitted without an available PM2.5 provider. See the [Home Assistant HomeKit Bridge documentation](https://www.home-assistant.io/integrations/homekit/).

When exposed as an air purifier through HomeKit Bridge, automatic maps to HA's `auto` preset. The current bridge sends a speed command when manual is selected, so it applies HomeKit's displayed speed. That speed command can also turn on a powered-off device. To restore the last manual speed while preserving power state, select manual directly in HA or the web page. The bridge can display zero-speed idle as purifying or minimum speed; use the device page to confirm actual idle state.

### M1 readings in Home Assistant / HomeKit

M1 publishes its own MQTT sensor entities: temperature, humidity, PM2.5, formaldehyde, and CO₂ or pressure when the corresponding extension sensors are installed. These entities can be selected in HomeKit Bridge. A1 uses M1 readings for automation and includes the required aggregate values in its own state. A1 no longer publishes proxy sensor entities for individual M1 units; upgrading clears their old discovery entries.

## Optional fresh-air fan

Starting with v0.9.23, choose `fresh-air` with embedded web resources or `fresh-air-no-frontend-resources` without them. Standard A1 units do not need a fresh-air variant. Use an image matching the current variant when upgrading. Fresh-air firmware occupies the user UART, which is no longer available for serial communication.

For a compatible PWM fan, connect user UART TX (GPIO9) to PWM control, RX (GPIO10) to tachometer feedback, and share signal ground. Power the fan from a supply matching its voltage and current requirements, never from signal pins. Check the fan pinout and signal levels before wiring and use level conversion when needed. Do not connect fan supply voltage to TX/RX. Original fan driver wiring remains unchanged; both PWM outputs run at 25 kHz.

The home page provides independent fresh-air power, speed and automatic mode. Fresh-air settings include PWM polarity, mapping and output limits. The normal preset increases duty with speed; the inverted preset decreases it. Stopping bypasses the minimum running duty. Feedback is displayed in RPM; set pulses per revolution to the fan specification, default 2. Feedback is informational and does not provide stall protection.

Choose independent speed, a fresh-air command limit proportional to the original fan, or an original-fan minimum proportional to fresh-air demand. In the last mode, original-fan automatic and night limits also constrain fresh air. Ratios apply to command percentages, not measured RPM. Master power-off stops both fans.

Both automatic policies share one settings page and are saved separately. Select one or more of PM2.5, formaldehyde and CO₂; CO₂ requires available sensor data.

Normal restart and firmware upgrade stop outputs first. Off levels during cold boot and bootloader execution are not guaranteed. If the fan must remain stopped throughout power-up, use suitable external pull resistors or an enable circuit and verify the actual electrical behavior.
