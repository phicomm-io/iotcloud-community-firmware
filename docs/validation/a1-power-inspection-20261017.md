# A1 power-key inspection validation (2026-10-17)

Scope: A1 b0f893303134 only. M1 is unavailable; sensor algorithm coverage uses the production MQTT simulator. TC1 and M1 receive build/regression coverage, without hardware flashing.

## Behavior

The default single power-key click powers on from off, preserving the selected mode. While running it displays cyan double pulses for automatic mode or a warm amber pulse for manual mode, followed by a same-color bar for actual applied airflow. Automatic idle displays an empty bar. The display does not save configuration or change power, mode, manual speed, automatic ceiling, or lighting settings. Long press for 1–4 seconds executes power-off on release; double click changes mode without changing power. Existing custom mappings are preserved.

The user's physical test confirmed short-press inspection, double-click mode selection, long-press shutdown, and short-press startup. Device I2C gesture logs and independently sampled states confirmed the same sequence. The user identified an extra green key flash competing with inspection; the corrected renderer suppresses generic green power-key feedback while running, including hold pulses, and retains it when pressing from off. Child-lock refusal stays red; Wi-Fi and combination-key feedback are preserved. Visual confirmation of this last correction is tracked separately from automated tests.

## Verification

- Host regressions: PASS, including v7 storage migration, custom-mapping preservation, action parsing and schedule rejection, default gestures, child lock, mode/power independence, actual zero-airflow bars, and night brightness limits.
- Real ARM GCC 5.4.1 build: PASS. Inspection / power-on stack path uses 7792 application bytes plus a 2048-byte reserve within its 12288-byte allocation.
- Green-feedback regression: PASS. Running power-key taps and held pulses leave the rendered mode/airflow frame unchanged; off-state green, locked red, Wi-Fi blue, and combination feedback remain covered.
- Simulator button regression: PASS, including inspection without configuration revision changes and user-defined gestures.
- Production MQTT regressions: PASS for discovery, commands, retained messages, independent power/mode, speed ceilings, hysteresis, fixed/linear/PID regulation, sensor fallback and recovery.
- Frontend forms and desktop/mobile browser regressions: PASS. A1 automatic settings can be saved twice from the same form using the acknowledged latest revision.
- Authenticated candidate IotCloud plus physical A1: PASS, two successive saves read back from the device while preserving off state, mode and ceiling; original thresholds restored.
- Initial signed candidate OTA: PASS; identity, runtime, panel, Wi-Fi/MQTT and zero hardware errors checked. Wi-Fi, MQTT, binding, linkage and night settings preserved.
- Physical test settings restored to the latest pre-test mode, power and remembered manual speed.
- Public Chinese/English guides and Wiki generation: PASS.

Candidate UI routing was used only for pre-release browser acceptance. Official release assets, production deployment, final firmware runtime and public-site verification must be recorded separately; candidate evidence alone does not establish deployment.
