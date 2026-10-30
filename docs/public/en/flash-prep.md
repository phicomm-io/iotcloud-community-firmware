# Before flashing

Before the first installation, check the power requirements and firmware type, then follow the wiring instructions for your model.

For OTA migration, keep the normal power supply as described in the model's guide. The wiring and independent-supply requirements below apply when opening a device and connecting a programmer.

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
| TC1 power strip | EMW3031 | Stock 1.0.0.24 / 1.0.0.26 OTA, or J-Link + FlashPlus | [Illustrated OTA](tc1-no-disassembly-ethernet-wifi-ap) · [Wired recovery](tc1-flash) |
| M1 air quality monitor | EMW3080B / MK3080B | Factory OTA; serial bootloader recommended for wired recovery, with accessible pads and a common USB-TTL adapter | [OTA installation](m1-no-disassembly) · [Wired recovery](m1-flash) |
| A1 air purifier | EMW3080B / MK3080B | Factory OTA, or solder-free J-Link / serial | [OTA installation](a1-no-disassembly) · [Wired recovery](a1-flash) |

The TC1 A1 hardware revision and the A1 air purifier are different products. Download firmware by product.

## Firmware file types

| File | Purpose | First installation? |
| --- | --- | --- |
| `.all.bin` | Native full Flash image | Yes, full-address programming |
| `.ota.bin.signed` | Signed OTA update package | No, only devices already running this firmware |
| `.ota.bin.raw` | Native OTA image | Use the full image for the matching product; M1 requires 0.9.21 or newer. Follow the product's guide |

**A1, M1, TC1 and DAIKIN community web updates use `.ota.bin.signed`; factory migration follows the model-specific guide.** S30-V1B uses separate RBL / STOCK_UA files; see [S30 installation](s30-flash). Verify downloads against `SHA256SUMS` from the same release.

## Wiring and instructions

- [TC1 flashing](tc1-flash): module pads, factory test points and FlashPlus.
- [M1 wired recovery](m1-flash): serial bootloader recommended, with accessible solder pads and commonly available USB-TTL adapters; J-Link is also covered.
- [A1 wired recovery](a1-flash): solder-free J-Link via factory test points recommended; serial is also covered.
- [Asset sources](sources) and [asset checksums](assets/SHA256SUMS).
