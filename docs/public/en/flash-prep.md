# Before flashing

Read these common requirements before following a model-specific guide. The individual guides describe wiring points and differences between products.

## Electrical safety

**Disconnect mains power, the original power supply and all loads before opening the device or connecting a programmer.** During flashing:

- Use an independent, stable **3.3 V supply**.
- **Never connect mains power and a computer programming cable at the same time.**
- **Never apply 5 V to the module's supply or signal pins.**

A full image overwrites the original firmware and some settings. Save a full Flash backup of **this particular device** before writing; another device's backup is not a substitute because it contains device-specific parameters.

**J-Link VTref is a voltage-sense input, not a 3.3 V power output. Pin 19 on the standard 20-pin connector supplies 5 V: do not connect it to the module.**

## Choose the correct procedure

| Device | Module | First-install method | Guide |
| --- | --- | --- | --- |
| TC1 power strip | EMW3031 | J-Link + FlashPlus, full image at Flash offset 0 | [TC1 flashing](tc1-flash) |
| M1 air quality monitor | EMW3080B / MK3080B | J-Link or serial bootloader | [A1 / M1 flashing](a1-m1-flash) |
| A1 air purifier | EMW3080B / MK3080B | J-Link or serial bootloader | [A1 / M1 flashing](a1-m1-flash) |

The TC1 A1 hardware revision and the A1 air purifier are different products. Download firmware by product.

## Firmware file types

| File | Purpose | First installation? |
| --- | --- | --- |
| `.all.bin` | Native full Flash image | Yes, full-address programming |
| `.ota.bin` / `.ota.bin.signed` | Signed OTA update package | No, only devices already running this firmware |
| `.ota.bin.raw` | Raw OTA image | No; serial image preparation or explicitly chosen unsigned recovery |

**Use the OTA file for a web update, never `.all.bin` or `.usr.bin`.** Verify downloads against `SHA256SUMS` from the same release.

## Wiring and instructions

- [TC1 flashing](tc1-flash): module pads, factory test points and FlashPlus.
- [A1 / M1 flashing](a1-m1-flash): module SWD / UART pins, A1 factory test points, J-Link and bootloader methods.
- [Asset sources](sources) and [asset checksums](assets/SHA256SUMS).
