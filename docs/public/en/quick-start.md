# Quick start

This community firmware supports the Phicomm TC1 power strip, M1 air quality monitor and A1 air purifier. It provides local web control, MQTT and Home Assistant integration. Firmware downloads are free; the manufacturer's cloud is not required.

## Identify your device

| Device | Functions | Hardware |
| --- | --- | --- |
| TC1 power strip | Six outlets, metering, child lock, indicators and schedules | A1 hardware revision, EMW3031. **A2 hardware is not supported.** |
| M1 air quality monitor | PM2.5, formaldehyde, temperature, humidity and screen brightness | Original EMW3080B / MK3080B module; retain the ATSAMD20 |
| A1 air purifier | Fan, light ring, child lock, night mode and M1 automation | Original EMW3080B / MK3080B module |

**`tc1(a1)` means the A1 hardware revision of the TC1 power strip, not the A1 air purifier. They are different products and cannot use each other's firmware.** Check the model before flashing.

For a first installation, follow [TC1 flashing](tc1-flash) (EMW3031, J-Link) or [A1 / M1 flashing](a1-m1-flash) (EMW3080B, J-Link or serial bootloader).

## First Wi-Fi setup

If Wi-Fi has not been configured, the device opens a setup access point. Connect a phone or computer and open `http://192.168.0.1/`.

| Device | Setup access point |
| --- | --- |
| TC1, A1 hardware revision | `TC1-AP-<last six MAC digits>` |
| M1 | `M1-AP-<last six MAC digits>` |
| A1 air purifier | `A1-AP-<last six MAC digits>` |

The six MAC digits are uppercase. Older firmware may use a different setup address; check its release notes.

## Open the device WebUI

After setup, find the device's DHCP address in your router and open that IP in a browser. The top bar offers Chinese / English and light / dark themes. Settings are grouped into network, device, automation and maintenance; each product shows only supported functions. Save changes explicitly. Some network changes disconnect the browser.

Theme preferences are stored locally in the browser and shared with the device UI on that origin.

## Next steps

- Daily operation: [TC1](tc1-guide), [M1](m1-guide), [A1](a1-guide).
- [Firmware updates](firmware-update).
- [MQTT, Home Assistant and automation](mqtt-link).
- [Troubleshooting](faq).
