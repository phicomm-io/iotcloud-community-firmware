# S30-V1B operation

The **ORVIBO S30-V1B** is a single-outlet metering socket with a BL2028N module, BK7231N chip and HLW8032 meter. The firmware uses the project's common home page, settings navigation, themes and language controls. Its project name is `iotcloud` and protocol name is `openiot`. For first installation, see [S30-V1B serial flashing](s30-flash).

Meter frame reception, live readings and increasing accumulated energy have been verified with the original power supply and a real load. The default coefficients have not been calibrated against reference instruments, so no accuracy claim is made. Do not use the current readings for billing or safety protection.

## Wi-Fi setup and access

1. With no network configured, join `S30M_V1B_XXXXXX`. `XXXXXX` is the last six digits of the factory MAC in uppercase hexadecimal. The initial AP has no password.
2. Open **http://192.168.0.1** and save an available **2.4 GHz Wi-Fi** network and its password in settings.
3. After the device restarts and connects, return your phone or computer to the same LAN. Find the device's assigned IP in the router and open it in a browser. Its hostname is `iotcloud-s30-v1b-xxxxxx`, with the MAC suffix in lowercase.

The initial web administration password is empty, so pages open directly without login. Authentication is required only after a management password is set; HTTP Basic clients use the username `admin`. This password is separate from Wi-Fi and MQTT credentials. A forgotten administration password requires a factory reset; a Wi-Fi reset preserves it.

## Outlet and settings

The home page provides one outlet switch and meter readings. A short button press also toggles output, and the blue LED follows output state. Child lock blocks short presses, while web control and long-press recovery remain available.

Settings cover device and outlet names, power-on state, indicators, child lock, reporting interval, Wi-Fi, MQTT and administration password. The default power-on state is off. After a restart, the saved power-on policy applies rather than necessarily restoring the previous temporary output state. Remote switching may return busy for about 1.5 seconds after a short press to avoid immediately overriding local operation.

## Red and blue indicators

The following applies while normal indicators are enabled:

| State | Blue | Red |
| --- | --- | --- |
| Wi-Fi connected; MQTT unconfigured or ready | Steady on for output on, off for output off | Off |
| Wi-Fi configured but disconnected | Continues to show output state | Fast blink, about 150 ms on and off |
| Wi-Fi connected; configured MQTT not ready | Continues to show output state | Slow blink, about 1 second on and off |
| No network configured and not connected | Alternates with red, about 500 ms per color | Alternates with blue |

Disabling indicators turns off these normal indications. Holding the button overrides them with timing feedback, even when indicators are disabled or child lock is enabled.

## Button and resets

| Operation | Action on release |
| --- | --- |
| Short press | Toggle output; ignored with child lock enabled |
| Hold for 1 second to under 5 seconds | No reset |
| Hold for at least 5 but under 10 seconds | Reset Wi-Fi and restart |
| Hold for at least 10 seconds | Factory reset and restart |

Long-press timing matches TC1: **one pulse per completed second, starting at 1 second, with about 160 ms on-time**. Pulses 1–4 are blue, 5–9 are red, and from 10 seconds both colors pulse together. Releasing restores normal indication. Resets execute on release; reaching 5 seconds does not immediately restart the device, allowing the 10-second gesture to complete.

A Wi-Fi reset clears the primary and backup networks, preserving product settings, MQTT, administration password, pairing, meter coefficients and accumulated energy. A factory reset clears all of these and returns to the default power-on-off policy. Join the device AP again to configure Wi-Fi.

## Metering

The HLW8032 provides voltage, current, active power and accumulated energy. Real-time fields appear only with valid samples. Missing samples or fields not updated for about 5 seconds appear as unavailable, which must not be interpreted as zero load power.

Default voltage, current and power coefficients are `1.88 / 1 / 1.88`; they are starting values only. Calibration requires a stable load and independent reference instruments for each measurement. A nameplate or approximate wattage does not establish accuracy. Energy is saved about every 5 minutes, when settings are saved and before a normal restart. Sudden power loss may lose roughly the latest 5 minutes of accumulated energy.

If output is on but readings remain unavailable, check metering validity rather than increasing coefficients to create a reading. Power off and remove the load before inspecting meter wiring. Mains must be disconnected before reconnecting UART or external DC power.

## MQTT and Home Assistant

Enter the MQTT broker address, port and required credentials in settings. TCP MQTT is supported. Home Assistant can discover six entities: the outlet, voltage, current, power, accumulated energy and meter validity. Local web use does not require MQTT.

HTTP, MQTT and UDP business messages use the project's `openiot` MessagePack / CBOR binary contract. JSON is reserved for Home Assistant. See `BL2028N/README.md` in the repository for topics, pairing scopes and interfaces. An empty web password does not remove MQTT broker authentication or UDP pairing.

## Firmware updates

After installing this firmware, enter an **HTTP download URL for this device's `.rbl` image** in the web upgrade settings. The device must be able to reach the URL directly. Obtain both image and matching checksums from a trusted source. During the update, MQTT, UDP, scanning and new control/configuration operations pause; HTTP status remains available. Failed downloads restore services; a successful update restarts automatically. The saved power-on policy applies after reboot, so first confirm that the load allows interruption and power restoration.

Local file upload and digital signature verification are not implemented. Other devices' signed `.ota.bin` packages are incompatible, and RBL integrity checking does not provide signature authentication. First installation still follows the [disassembly guide](s30-flash).
