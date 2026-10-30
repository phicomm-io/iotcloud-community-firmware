# S30-V1B features

The **ORVIBO S30-V1B** single-outlet meter uses BL2028N / BK7231N and HLW8032. See [common functions](s30-guide) for operation and [automation and remote display](s30-automation) for power rules and M1 fields.

## Power restoration: off / on / restore

Published RC4 supports off and on. Restore requires the matching development firmware.

In Settings → General → Power-on policy, choose:

| Policy | Output after power restoration / reboot |
| --- | --- |
| Off (default) | Off |
| On | On |
| Restore | Last successfully saved relay state |

Restore saves relay state, not an in-progress automation timer. A physical short press acts first and saves in the background, so an immediate power cut may lose that last press. Remote and automatic changes save the state before switching output; a failed save leaves output unchanged. Selecting restore captures the current output. Upgrading old community settings preserves networking, authentication, names, calibration and energy, and maps the existing on / off policy without changing its meaning.

## Red and blue indicators

The following applies while normal indicators are enabled:

| State | Blue | Red |
| --- | --- | --- |
| Wi-Fi connected; MQTT unconfigured or ready | Steady on for output on, off for output off | Off |
| Wi-Fi configured but disconnected | Continues to show output state | Fast blink, about 150 ms on and off |
| Wi-Fi connected; configured MQTT not ready | Continues to show output state | Slow blink, about 1 second on and off |
| No network configured and not connected | Alternates with red, about 500 ms per color | Alternates with blue |

Disabling indicators turns off these normal indications. Holding the button overrides them with timing feedback, even when indicators are disabled or child lock is enabled.

## Local button and child lock

A short press toggles the relay. Child lock blocks ordinary short presses, not recovery gestures or hold-time feedback. Remote control waits for about 1.5 seconds after a short press. Recovery instructions are under [common functions → recovery](s30-guide#recovery).

## Metering

The HLW8032 provides voltage, current, active power and accumulated energy. Real-time fields appear only with valid samples. Missing samples or fields not updated for about 5 seconds appear as unavailable, which must not be interpreted as zero load power.

The default voltage, current and power multipliers are `1.894288 / 1.0102 / 1.88835008906368`, extracted from fixed conversion constants in this model's stock `2M.1220` firmware. Use independent reference instruments when adjusting calibration.

Open **Settings → General → Calibration** to edit the three multipliers separately. Each accepts a finite value from `0.001` to `100`, retaining the full decimal precision shown above. Saving applies the values immediately and keeps them after a restart. Calibrate with a stable load, comparing each measurement against independent reference instruments. “Calibrated” only means that calibration parameters have been saved by a user.

Upgrades replace the old `1.88 / 1 / 1.88` defaults only when they have never been saved as a calibration. Manually saved multipliers, other combinations and accumulated energy are preserved. A Wi-Fi reset keeps the multipliers; a factory reset restores the stock model defaults above. Energy is saved about every 5 minutes, when settings are saved and before a normal restart. Sudden power loss may lose roughly the latest 5 minutes of accumulated energy.

If output is on but readings remain unavailable, check metering validity rather than increasing coefficients to create a reading. Power off and remove the load before inspecting meter wiring. Mains must be disconnected before reconnecting UART or external DC power.
