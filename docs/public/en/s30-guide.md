# S30-V1B common functions

Daily use, updates and recovery after installation. Choose stock network or UART installation in [flashing methods](s30-flash). See [device features](s30-features) for power restoration, LEDs and calibration, and [automation and field IDs](s30-automation) for power conditions and M1 display.

## Wi-Fi setup and access

1. With no network configured, join `S30M_V1B_XXXXXX`. `XXXXXX` is the last six digits of the factory MAC in uppercase hexadecimal. The initial AP has no password.
2. Open **http://192.168.0.1** and save an available **2.4 GHz Wi-Fi** network and its password in settings.
3. After the device restarts and connects, return your phone or computer to the same LAN. Find the device's assigned IP in the router and open it in a browser. Its hostname is `iotcloud-s30-v1b-xxxxxx`, with the MAC suffix in lowercase.

The initial web administration password is empty, so pages open directly without login. Authentication is required only after a management password is set; HTTP Basic clients use the username `admin`. This password is separate from Wi-Fi and MQTT credentials. A forgotten administration password requires a factory reset; a Wi-Fi reset preserves it.

## Outlet and settings

The home page provides one outlet switch, voltage (V), current (A), active power (W) and accumulated energy. Iotcloud device cards and device details also show voltage and current from the same status data, without adding meter polling. A short button press also toggles output, and the blue LED follows output state. Child lock blocks short presses, while web control and long-press recovery remain available.

Settings cover device and outlet names, power-on state, indicators, child lock, reporting interval, Wi-Fi, MQTT and administration password. The default power-on state is off. The reissued RC5 supports off, on and restoration of the last saved output state; earlier RC4 builds support off and on; see [power restoration](s30-features#power-restoration-off-on-restore). Remote switching may return busy for about 1.5 seconds after a short press to avoid immediately overriding local operation.

## Updates

<span id="firmware-updates"></span>

After installing this firmware, enter an **HTTP download URL for this device's `.rbl` image** in the web upgrade settings. The device must be able to reach the URL directly. Obtain both image and matching checksums from a trusted source. During the update, MQTT, UDP, scanning and new control/configuration operations pause; HTTP status remains available. Failed downloads restore services; a successful update restarts automatically. The saved power-on policy applies after reboot, so first confirm that the load allows interruption and power restoration.

Updates support URL downloads only. RBL images are unsigned: verify SHA-256 and use a trusted download source. Other devices’ `.ota.bin` packages are incompatible. For first installation and recovery, see [network and serial installation](s30-flash).

Download the RBL and matching checksums to one directory and verify the image. Run `python3 -m http.server 8000 --bind 0.0.0.0` there, allow the port through the firewall, and enter `http://<computer-LAN-IP>:8000/BL2028N_OTA_<version>.rbl` on the device. Do not use `localhost` or a download description page. Stop the temporary server after the upgrade.

## Recovery

<span id="button-and-reset"></span>
<span id="button-and-resets"></span>

| Operation | Action on release |
| --- | --- |
| Short press | Toggle output; ignored with child lock enabled |
| Hold for 1 second to under 5 seconds | No reset |
| Hold for at least 5 but under 10 seconds | Reset Wi-Fi and restart |
| Hold for at least 10 seconds | Factory reset and restart |

Long-press timing matches TC1: **one pulse per completed second, starting at 1 second, with about 160 ms on-time**. Pulses 1–4 are blue, 5–9 are red, and from 10 seconds both colors pulse together. Releasing restores normal indication. Resets execute on release; reaching 5 seconds does not immediately restart the device, allowing the 10-second gesture to complete.

A Wi-Fi reset clears the primary and backup networks, preserving product settings, MQTT, administration password, pairing, meter coefficients and accumulated energy. A factory reset clears all of these and returns to the default power-on-off policy. Join the device AP again to configure Wi-Fi.


## MQTT and Home Assistant

Enter the MQTT broker address, port and required credentials in settings. TCP MQTT is supported. Home Assistant can discover six entities: the outlet, voltage, current, power, accumulated energy and meter validity. Local web use does not require MQTT.

HTTP, MQTT and UDP business messages use the project's `openiot` MessagePack / CBOR binary contract. JSON is reserved for Home Assistant. See `BL2028N/README.md` in the repository for topics, pairing scopes and interfaces. MQTT uses broker credentials; UDP requires pairing.


<span id="red-and-blue-indicators"></span>
<span id="meter-readings"></span>

LED and meter calibration details are in [S30-V1B features](s30-features).
