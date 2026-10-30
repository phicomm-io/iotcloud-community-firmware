# TC1 wired installation and recovery (A1 hardware revision)

Only for the TC1 power strip with A1 hardware and an EMW3031 module. TC1 A2 is unsupported. Read [Before flashing](flash-prep) first.

This procedure opens the case and uses J-Link. Stock `1.0.0.26` / `1.0.0.24` can use the [illustrated OTA guide](tc1-no-disassembly-ethernet-wifi-ap) instead.

## Tools and opening the case

Use a Windows computer, SWD-capable J-Link, [SEGGER drivers](https://www.segger.com/downloads/jlink/), FlashPlus, an independent 3.3 V supply, wires, suitable screwdrivers and soldering tools. FlashPlus uses Marvell's EMW3031 configuration. Get it from [asset sources](sources); the [EMW3031 / FlashPlus manual mirror](https://github.com/IoTDevice/phicomm_tc1_a1/blob/master/工具和文档/EMW3031说明文档_flashplus.pdf) describes its settings and Flash layout.

![FlashPlus chip and Flash settings](assets/images/flashplus-config.png)

Remove the waterproof pads underneath and the screws. Gently tap the middle of the underside until the case separates. Remove the board and confirm the module is EMW3031. Keep mains disconnected.

## SWD wiring

Choose either module pads or factory test points; they use the same signals and need not both be connected. Follow the labels, antenna orientation and board markings, not wire colors or mirrored left / right positions.

![EMW3031 module: 3.3V, GND, DIO, CLK](assets/images/tc1-a1-module.png)

The factory test points in the red box are 3V3, DIO, CLK and GND. The text in the photo is upside down: check each marking. Use reliable probes, a fixture or soldered wires; never move them during erase / write.

![TC1 A1 factory test points](assets/images/tc1-a1-test-points.png)

| Module / board signal | Standard 20-pin J-Link connector |
| --- | --- |
| Pin 16, 3V3 | Independent 3.3 V supply and pin 1, VTref |
| Pin 17, GND | Pin 4, GND, common with supply negative |
| Pin 1 or 3, SWD_CLK / CLK | Pin 9, SWCLK |
| Pin 2 or 4, SWD_DIO / DIO | Pin 7, SWDIO |
| Pin 11, RESET, if needed | Pin 15, nRESET |

See the [EMW3031 datasheet pin diagram, PDF page 8](https://supp.iczoom.com/images/public/20190118/1547790131015026781.pdf#page=8).

![Official J-Link SWD connector](assets/images/jlink-swd.png)

**VTref senses voltage; it does not supply 3.3 V. Do not connect J-Link pin 19 (5 V) to the module.** Compact programmers may have different pin layouts; follow their signal labels.

## Programming

1. Disconnect mains. Connect the SWD wires and independent 3.3 V supply, then J-Link to the computer. Open FlashPlus.
2. Select **Chip = 88MW30X**, **Interface = JLINK**, **External Flash**, **QSPI**, **Type = Other**.
3. Back up first: select **READ**, Flash address `0`, Length of byte `200000` (hexadecimal, 2 MiB), then **Save to file**. A valid full backup is **2,097,152 bytes**. If reading fails or the length differs, stop and fix the problem before writing. Keep this device-specific backup private.
4. Select **PROGRAM**, Flash Start Address `0`, and `phicomm_tc1_a1.all.bin`. Use **Erase flash sectors by file**, **not Erase all flash**. Enable **Verify after programming**.
5. The current full image must end before `0x1E0000`. If it exceeds that boundary, stop and check that release's layout. Click **Start** and wait for successful programming and verification before disconnecting. Never substitute a CPU-mapped source address for the Flash offset.
6. Remove programming wires with power off, reassemble, then power normally. Join `TC1-AP-<last six MAC digits>` and open `http://192.168.0.1/`.

## Troubleshooting and factory recovery

If detection fails, check 3.3 V, VTref, common ground, CLK / DIO and drivers; try a lower J-Link speed. If writing or verification fails, keep mains disconnected, repair the wiring and reprogram.

To restore factory firmware, write and verify a full backup from **the same device** at Flash offset `0`. Without that backup, do not erase the whole chip or write another device's parameters.

Continue with [Wi-Fi setup](quick-start#first-wi-fi-setup), [TC1 operation](tc1-guide) and [OTA updates](firmware-update).
