# S30-V1B network and serial installation

This guide covers the **ORVIBO S30-V1B**, with a BL2028N module, BK7231N chip and HLW8032 meter. Network installation has been verified on stock **`2M.1220`**. UART remains available for backups, installation and recovery. The default layout retains this unit’s original bootloader and factory MAC/RF; **first installation starts with fresh community settings and requires Wi-Fi setup again**. See [S30-V1B operation](s30-guide) for use after installation. An identical enclosure does not establish compatibility with other S30 variants or modules.

## Choose an installation method

| Current state | Method | File |
| --- | --- | --- |
| Stock `2M.1220`, already connected to the same reachable LAN | Network installation below | `BL2028N_OTA_<version>.rbl` |
| This project's firmware with the stock layout and a working web interface | Settings → Firmware: enter the RBL HTTP URL | Same RBL |
| No network, damaged application, or a full backup is needed | Open the enclosure and use UART | `BL2028N_STOCK_UA_<version>.bin` at `0x11000` |

The web interface currently supports **URL download only, not local file upload**. Downloading a file to a computer does not make it available to the device. The default build does not emit QIO images containing another bootloader. A raw `BL2028N_app_*.bin` or UG package cannot replace the files above.

## Network installation from stock firmware

### Scope and prerequisites

This method completed a real installation on one **S30-V1B / BL2028N / BK7231N running stock `2M.1220`**. Other stock versions and S30 variants remain unverified. The stock unit must already be connected to Wi-Fi, reachable from the computer, and its factory MAC must be known. This procedure does not replace stock first-time Wi-Fi provisioning. UART was used to establish the test baseline and observe/read back the result; the actual upgrade command and download used normal network interfaces.

The computer needs Python, `paho-mqtt`, a local HTTPS certificate and a **dedicated temporary TLS MQTT broker**. Plain MQTT did not complete connection on this stock version; `mqtt://` is not a substitute for `ssl://`. Router DNS interception is unnecessary. Use temporary broker credentials and keep that broker off the public Internet.

The procedure redirects the stock device's CGW/cloud configuration to these test services. Community first boot uses fresh settings: stock accounts, Wi-Fi, MQTT, pairing and meter settings are not migrated. Record any settings that must be entered again.

### Run the local installation tool

Use [s30_stock_ota.py](https://github.com/phicomm-io/iotcloud-community-firmware/blob/hotfix/rc4-selected-integration/tools/s30_stock_ota.py), reviewed alongside the candidate firmware. The computer, stock device and broker IPv4 addresses must be mutually reachable. Allow TCP `15683` for HTTPS CGW, `8080` for the firmware download, and the broker's selected port. Certificates must include their service IP; `--mqtt-ca` verifies the broker certificate on the computer.

Prepare two private JSON files with mode `600`; do not commit or publish them:

```json
{"ssid":"Your 2.4 GHz Wi-Fi","password":"Wi-Fi password"}
```

```json
{"host":"Temporary broker IPv4","port":18892,"username":"short-test-user","password":"short-test-password"}
```

MQTT usernames and passwords must each be 1–31 bytes. Keep addresses short and use dedicated test credentials; the tool does not exploit the identified stock length overflow.

Obtain the matching RBL and `SHA256SUMS`, verify them, and replace every placeholder:

```sh
chmod 600 wifi.private.json mqtt.private.json
sha256sum -c SHA256SUMS
python3 tools/s30_stock_ota.py \
  --device <stock-device-IP> --device-id <this-unit-12-digit-factory-MAC> \
  --host <computer-LAN-IP> \
  --wifi-file wifi.private.json --mqtt-file mqtt.private.json \
  --mqtt-ca broker-cert.pem --cert gateway-cert.pem --key gateway-key.pem \
  --rbl 'BL2028N_OTA_<version>.rbl' --sha256 <published-file-SHA256>
```

The tool configures local HTTPS CGW using CoAP `/qlink/netinfo`. Stock firmware retrieves the temporary TLS MQTT connection over HTTPS, receives a normal AndLink file command, and downloads the RBL. It does not access UART or write factory parameters. Returning to stock firmware and dual boot are not provided.

### Confirm installation

1. After complete HTTP delivery, wait for automatic installation and reboot. **File delivery alone does not prove installation.** Do not interrupt power at this point.
2. Join the community `S30M_V1B_XXXXXX` AP, open **http://192.168.0.1**, and configure Wi-Fi again.
3. Open the assigned LAN IP. Confirm `iotcloud · S30-V1B`, the selected version, outlet control and metering. An unset administration password must not trigger login.
4. Stop the dedicated test broker and remove temporary certificates and credentials. Computer services are no longer needed; future upgrades use the community URL OTA interface.

The physical test also completed a second community network OTA. Full readback confirmed the installed application and preservation of the original boot area and MAC/RF. This does not establish compatibility with every stock version, power-loss recovery or return-to-stock support. If no download or community AP appears, retain the errors and use the UART backup/recovery path below rather than trying unrelated images.

## Disassembly and UART

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
BL2028N_STOCK_UA_<version>.bin
Write start: 0x11000
```

`STOCK_UA` is the zero-key, CRC-framed application image for the stock layout. It ends at `0x132000` and includes the stock application footer, but contains **no bootloader or unit-specific factory data**. The chip remains BK7231N. Older `BK7231M_UA` packages use different partition/footer positions and are not interchangeable. If another bootloader was installed, restore and verify this unit’s backed-up boot area first: writing an application cannot replace it.

After verifying the firmware, replace the version placeholder below and run the command. Reset the module once the tool is waiting:

```sh
bk7231tools write_flash -d /dev/ttyUSB0 -b 115200 --timeout 60 -s 0x11000 -S 0 'BL2028N_STOCK_UA_<version>.bin'
```

Writing the UA application image at `0x11000` retains the unit's bootloader, factory RF/TLV and configuration tail. **Do not erase the whole chip or substitute an `.rbl`, `.UG.bin` or another product's `.ota.bin`.** QIO images include a bootloader and are not the default method in this section.

After the tool verifies the write, read the full Flash again and compare the written range with the UA file. Check that data outside that range, especially the bootloader and factory parameters, remains unchanged. On the validated unit, the RF/TLV sector is at `0x1e0000`; preserve its valid factory MAC. A completed transfer alone does not establish a working installation.

## First boot and recovery

1. After flashing, cycle the isolated 3.3 V supply and check for an AP named `S30M_V1B_` followed by the last six MAC digits in uppercase hexadecimal.
2. Connect and open **http://192.168.0.1**. Follow the [operation guide](s30-guide), configure Wi-Fi, and confirm the model and version. Both the initial AP password and web administration password are empty.
3. Power off, remove every UART and external supply wire, inspect the connection area, and restore the complete insulated enclosure before reconnecting the original power supply and load.

An isolated 3.3 V bench supply can verify boot, networking and the web interface. It does not establish load metering with the original metering circuit powered normally.

For handshake failures, check port ownership, crossed TX/RX, common ground, contact reliability, supply and reset timing. If the flashed unit will not boot, retain the backup and complete tool log, then check image, offset and bootloader key format. An interrupted previous OTA may require version-specific cleanup of pending upgrade data; do not try repeated whole-chip erases or another unit's RF/MAC data.
