# S30-V1B disassembly and serial flashing

This guide covers the **ORVIBO S30-V1B**, with a BL2028N module, BK7231N chip and HLW8032 meter. First installation currently requires **opening the enclosure and flashing over UART**. See [S30-V1B operation](s30-guide) for use after installation. An identical enclosure does not establish compatibility with other S30 variants or modules.

## Power and preparation

Unplug the socket from mains, disconnect the load and original power supply, and confirm power is off before opening it. **Keep mains disconnected whenever the enclosure is open, the computer UART is connected, or flashing is in progress.** Use a USB-to-TTL adapter with 3.3 V logic and a stable, separate, isolated 3.3 V DC supply. Do not apply 5 V to the module.

The adapter's 3.3 V output may not supply enough current when Wi-Fi starts, causing resets even after a successful flash. With a separate supply, leave the adapter's 3.3 V power wire disconnected. Retain TX, RX and GND, joining supply negative, adapter GND and device GND.

Obtain firmware and `SHA256SUMS` for this exact device and version, and verify the file first. Close serial applications such as minicom so that only the flashing tool owns the port. The examples use [bk7231tools](https://github.com/tuya-cloudcutter/bk7231tools#usage); replace `/dev/ttyUSB0` with the actual port.

## Contact locations and wiring

**Look directly at the PCB on the plug side.** At the **outer upper-right edge**, two contacts connect the power board to the MCU module: **the left contact is GND and the right contact is 3V3**. Use the board's **VTX / VRX** UART test points; some markings include a suffix, such as VTX1 / VRX1.

| Device contact | Connect to |
| --- | --- |
| VTX test point | USB-to-TTL RXD |
| VRX test point | USB-to-TTL TXD |
| Left contact of the pair at the outer upper-right, viewed from the plug side: GND | Supply negative and USB-to-TTL GND |
| Right contact of the same pair: 3V3 | Positive output of the separate isolated 3.3 V supply |

Left and right apply only to that viewing direction. Check orientation and contacts before wiring; turning the PCB over reverses the view. VTX/VRX describe the device's signal direction, so cross TX and RX.

## Make a full backup first

Start the tool and let it wait for a connection, then **cycle the module's 3.3 V power**, or briefly ground and release a positively identified CEN/RESET point. Keep the USB adapter connected. The socket's normal power button does not replace a chip reset.

The validated unit has 2 MiB of Flash, covering `0x000000` through `0x1fffff`. Read it twice, resetting while each command waits, and keep the tool's default checksum verification enabled:

```sh
bk7231tools read_flash -d /dev/ttyUSB0 -b 115200 --timeout 60 -s 0 -l 0x200000 s30-stock-1.bin
bk7231tools read_flash -d /dev/ttyUSB0 -b 115200 --timeout 60 -s 0 -l 0x200000 s30-stock-2.bin
sha256sum s30-stock-1.bin s30-stock-2.bin
cmp s30-stock-1.bin s30-stock-2.bin
```

Both files must contain 2097152 bytes and have identical SHA-256 hashes. `cmp` must exit successfully with no output. Save both backups and hashes before writing. They contain this unit's MAC, RF parameters and possibly factory credentials; keep them private. Another unit's backup is not a substitute.

## Select and write the image

The BL2028N unit validated here uses a **zero-key bootloader**. To retain that bootloader, use:

```text
BL2028N_BK7231M_UA_<version>.bin
Write start: 0x11000
```

Here, `BK7231M` in the filename identifies zero-key image packaging; **the chip remains BK7231N**. Do not select the regular N-key image solely from the chip name. Verify the bootloader/encryption format on other batches rather than assuming every BL2028N matches this sample.

After verifying the firmware, replace the version placeholder below and run the command. Reset the module once the tool is waiting:

```sh
bk7231tools write_flash -d /dev/ttyUSB0 -b 115200 --timeout 60 -s 0x11000 -S 0 'BL2028N_BK7231M_UA_<version>.bin'
```

Writing the UA application image at `0x11000` retains the unit's bootloader, factory RF/TLV and configuration tail. **Do not erase the whole chip or substitute an `.rbl`, `.UG.bin` or another product's `.ota.bin`.** QIO images include a bootloader and are not the default method in this section.

After the tool verifies the write, read the full Flash again and compare the written range with the UA file. Check that data outside that range, especially the bootloader and factory parameters, remains unchanged. On the validated unit, the RF/TLV sector is at `0x1e0000`; preserve its valid factory MAC. A completed transfer alone does not establish a working installation.

## First boot and recovery

1. After flashing, cycle the isolated 3.3 V supply and check for an AP named `S30M_V1B_` followed by the last six MAC digits in uppercase hexadecimal.
2. Connect and open **http://192.168.0.1**. Follow the [operation guide](s30-guide), configure Wi-Fi, and confirm the model and version. Both the initial AP password and web administration password are empty.
3. Power off, remove every UART and external supply wire, inspect the connection area, and restore the complete insulated enclosure before reconnecting the original power supply and load.

An isolated 3.3 V bench supply can verify boot, networking and the web interface. It does not establish load metering with the original metering circuit powered normally.

For handshake failures, check port ownership, crossed TX/RX, common ground, contact reliability, supply and reset timing. If the flashed unit will not boot, retain the backup and complete tool log, then check image, offset and bootloader key format. An interrupted previous OTA may require version-specific cleanup of pending upgrade data; do not try repeated whole-chip erases or another unit's RF/MAC data.
