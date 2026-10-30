# A1: wired installation and recovery

For the A1 with the original EMW3080B / MK3080B Wi-Fi module. Use only `A1` firmware files. For installation over Wi-Fi, see [A1 stock OTA](a1-no-disassembly).

Read [Before flashing](flash-prep). Disconnect mains, original supplies and loads, use a stable independent **3.3 V** supply, and back up the complete Flash before writing. A full image overwrites the bootloader, original firmware and some settings.

**Recommended: J-Link.** The A1 factory DIO / CLK and JP9 power points accept jumper wires, so soldering is unnecessary. Serial Bootloader remains an alternative.

## Choose a method

| Method | Requirements |
| --- | --- |
| J-Link | J-Link plus OpenOCD (Windows / Linux); validates and writes full-image segments for first installation / recovery. Factory points can accept jumper wires |
| Serial bootloader | 3.3 V USB-TTL and YMODEM software; original bootloader must work. No J-Link is required |

| Product | J-Link full image | Input for preparing serial images |
| --- | --- | --- |
| A1 purifier | `phicomm_a1.all.bin` | `phicomm_a1.ota.bin.raw` |

Download the correct product and verify `SHA256SUMS` from that release. `.ota.bin` / `.ota.bin.signed` are signed web-update packages, not full images or directly transferable serial user applications.

## J-Link flashing

### A1 factory points

A1's JP9 GND / 3V3 supply points and nearby DIO / CLK points avoid soldering to module pads. JP9 also has UART TX / RX: do not confuse them with SWD. In the red box, **DIO is above CLK in the photo orientation**. Check the photo and silkscreen; make firm jumper / header contacts.

![A1 factory programming points](assets/images/a1-jlink-points.png)

VCC here means module **3.3 V**, not the original supply input. Disconnect the original supply and fan. Wire colors are only visual references.

### Module pads

A1 can use wires soldered to its module pads. Check orientation by pin numbers.

![EMW3080B SWD pins](assets/images/emw3080-swd-pins.png)

| Module pin | Connection |
| --- | --- |
| 1 or 3, SWD_CLK | J-Link SWCLK, standard 20-pin pin 9 |
| 2 or 4, SWD_DIO | J-Link SWDIO, standard pin 7 |
| 16, VCC | Independent 3.3 V and J-Link VTref, standard pin 1 |
| 17, GND | Common supply negative and J-Link GND, standard pin 4 |

VTref is a sensing input, not a power output. Do not connect J-Link's 5 V output. Use signal labels for compact programmers rather than assuming standard pin numbers. SWD flashing does not require grounding BOOT.

![Standard J-Link SWD connector](assets/images/jlink-swd.png)

### Prepare OpenOCD and the helper scripts

Use **OpenOCD + J-Link over SWD**. No mlink / mflash application is needed. Windows uses the legacy OpenOCD included in the tool package; Linux uses OpenOCD 0.11 / 0.12 with the new configuration. Both need the matching `flash_prog.elf`; installing stock OpenOCD and running `program all.bin` is insufficient.

Download and extract into a short working path, such as `C:\phicomm` or `~/phicomm`:

- [EMW3080 helper scripts](assets/tools/emw3080-openocd.zip): Python 3 validation, splitting and flashing helper, plus a modern OpenOCD configuration.
- [zA1 3080_JLINK tool package](https://github.com/a2633063/zA1/raw/f4f67ee88d9287fd1badff55ccd7704bb0f748c4/Jlink烧录文档/3080_JLINK烧录.zip): retain `openocd/`, `sflash_write.tcl` and `flash_prog.elf`, and rename the outer directory to `3080-tool`. Do not run its old firmware or batch files.
- Your product's `.all.bin` and `SHA256SUMS` from the **same release**. Signed web packages and other products are not accepted.

Extract the helper files into the working directory, outside `3080-tool`:

```text
phicomm/
  emw3080.py
  mx1290-modern.cfg
  phicomm_a1.all.bin
  SHA256SUMS
  3080-tool/
    flash_prog.elf
    sflash_write.tcl
    openocd/
      win32/openocd_mico.exe
      scripts/interface/jlink_swd.cfg
      scripts/target/MX1290/...
```

**Back up before writing.** The helper does not provide complete Flash backup or factory restoration. Use an appropriate full-read tool first if factory recovery is required. The old package's `sflash_read_file` has argument and external-command problems and must not be used as a cross-platform backup command. The connection check identifies the debug interface without writing Flash. The flash command overwrites the bootloader, kernel, application and ATE.

### Windows: install, check and flash

1. Install [Python 3](https://www.python.org/downloads/windows/) version 3.9 or newer. Confirm `py -3 --version` works.
2. OpenOCD accesses J-Link through libusb. If it already detects J-Link, keep the driver. For `LIBUSB_ERROR_NOT_SUPPORTED`, follow the [OpenOCD Windows instructions](https://github.com/openocd-org/openocd/blob/master/README.Windows.md) and use [Zadig](https://zadig.akeo.ie/) to install WinUSB **for J-Link only**. SEGGER applications may need their original driver restored afterward.
3. With power off, connect SWD, VTref, common ground and independent 3.3 V power. Power the module and close other J-Link applications. Open PowerShell in the working directory and check the connection:

```powershell
cd C:\phicomm
py -3 .\emw3080.py check --tool-dir .\3080-tool --engine legacy
```

Look for approximately 3.3 V `Vref`, a SWD ID and `MX1290.cpu`, followed by a successful exit. Fix wiring, power or driver problems before proceeding.

For **A1**:

```powershell
py -3 .\emw3080.py flash .\phicomm_a1.all.bin --model a1 --checksums .\SHA256SUMS --tool-dir .\3080-tool --engine legacy
```

### Linux: install, check and flash

On Debian / Ubuntu:

```sh
sudo apt update
sudo apt install openocd python3
openocd --version
cd ~/phicomm
python3 emw3080.py check --tool-dir ./3080-tool --engine modern
```

Use **OpenOCD 0.11 / 0.12**. This mode loads the helper's `mx1290-modern.cfg`, not the legacy package's `MX1290.cfg`. For USB permission errors, check your distribution's OpenOCD udev rules and group permissions, log in again and reconnect J-Link. Install equivalent packages and USB rules on other distributions.

For the A1 purifier:

```sh
python3 emw3080.py flash phicomm_a1.all.bin --model a1 --checksums SHA256SUMS --tool-dir ./3080-tool --engine modern
```

Use `--openocd /absolute/path/openocd` if OpenOCD is outside PATH. The Linux configuration supports the newer DAP interface; this procedure remains experimental.

### Split files and verify flashing

Before accessing J-Link, `flash` validates the release SHA256, product, image boundaries, application CRC and embedded raw OTA MD5, then splits and writes the image. Validation failure exits without erasing. Only this project's current `.all.bin` layout is supported. Original tool files are also checked to prevent mixing in a programmer for another board.

To split without connecting a device (replace `python3` with `py -3` on Windows):

```sh
python3 emw3080.py split phicomm_a1.all.bin --model a1 --checksums SHA256SUMS --output firmware
```

The output directory must not exist; existing files are never overwritten.

| Output | Full-image range (end exclusive) | Programmer partition ID | Offset within partition |
| --- | --- | --- | --- |
| `boot.bin` | `0x000000`–`0x012000` (72 KiB) | `1` | `0x0` |
| `ota.bin` | `0x013000`–`0x0C8000` (724 KiB including padding) | `2` | `0x0` |
| `ate.bin` | `0x0D0000`–end of file (up to 256 KiB) | `3` | `0x0` |

The split `ota.bin` is for this SWD process only; do not upload it to the web updater. The three writes exclude `0x012000`–`0x013000` and parameters at `0x0C8000`–`0x0D0000`. **Partition IDs are not raw Flash addresses.** Never change the three `0x0` arguments to `0x13000` or `0xD0000`.

The helper runs these operations using erase-as-needed and programmer post-write verification, without whole-chip erase:

```text
sflash_write_file firmware/boot.bin 1 0x0 MK3080B 0
sflash_write_file firmware/ota.bin 2 0x0 MK3080B 0
sflash_write_file firmware/ate.bin 3 0x0 MK3080B 0
reset run
shutdown
```

`flash` uses an automatically cleaned temporary directory. Every chunk must return `Result: OK` and OpenOCD must exit successfully. `Verify after write failed`, `Error during Write` or a nonzero exit means failure: stabilize power and repair the connection before rewriting. Then check normal boot, provisioning and device functions as described below.

| Problem | Action |
| --- | --- |
| Missing `flash_prog.elf` / tool hash mismatch | Point `--tool-dir` at the original package root and retain its matching files; do not substitute another board's ELF |
| `target requires -dap` | Use `--engine modern` with the included `mx1290-modern.cfg` for newer OpenOCD |
| `couldn't bind to socket` | The helper uses `23333 / 24444 / 26666`. Check occupied or Windows reserved ports with `netsh interface ipv4 show excludedportrange protocol=tcp` |
| Legacy `Named pipes currently not supported` | Use numeric ports; do not set legacy `gdb_port` to `disabled` |
| Verification succeeds but no setup hotspot | Power off, remove programming wires, restore normal wiring and power; check the product image and collect boot logs if necessary. Do not try whole-chip erase |

## Bootloader flashing

Use a **3.3 V, 921600-baud** USB-TTL adapter, independent 3.3 V supply and YMODEM-capable terminal. Disconnect the original supply and fan.

### Wiring and download mode

![EMW3080B serial bootloader pins](assets/images/emw3080-pins.png)

| Module pin | Connection |
| --- | --- |
| 16, VCC | Independent 3.3 V |
| 17, GND | Supply negative and USB-TTL GND |
| 9, user UART TX | USB-TTL RX |
| 10, user UART RX | USB-TTL TX |
| 19, BOOT | Ground during download; release afterward |

Cross TX / RX. This is the **user UART, not the log UART**. If another board controller drives module RX, isolate its output first to avoid two TX drivers. Disconnect the original supply and fan.

Wire with power off and BOOT low. Use **921600, 8N1, no flow control**, then power on. Wait for `MXCHIP>`. If it does not appear, check power, ground, TX / RX and BOOT; never send erase commands blindly. This method preserves the original bootloader and applies only to the menu and partition layout below.

### 1. Confirm the menu and partition map

Select the USB-TTL port with 921600, 8N1, no hardware or software flow control. Enter `?`:

```text
1:FWUPDATE     Update MiCO
2:USERUPDATE   Update user APP
5:MEMORYMAP
6:BOOT
```

Enter `5` and verify the key partitions. These are **Flash offsets, not CPU-mapped addresses**.

| ID | Partition | Start | Length |
| --- | --- | --- | --- |
| 2 | Bootloader | `0x00000000` | `0x00013000` |
| 3 | MiCO Kernel | `0x00013000` | `0x000b5000` |
| 6 | User Application | `0x00088000` | `0x00040000` |
| 7 | PARAMETER1 | `0x000c8000` | `0x00004000` |
| 8 | Parameter backup, also labeled PARAMETER1 | `0x000cc000` | `0x00004000` |

The MiCO region includes the user application, at offset `0x75000` from MiCO's start. **Stop if the menu or map differs.** This procedure does not write the bootloader, parameters, ATE, OTA Storage or USER Storage. Replacing the kernel / application still replaces factory firmware; preserving those regions does not replace a full backup.

### 2. Prepare serial images

Verify the product's raw OTA file against the release checksums. The script fixes `model = 'a1'`. **Never send `.all.bin` to menu 1 or 2, or an unconverted OTA file to menu 2.**

Raw `.ota.bin.raw` contains `0x75000` bytes of kernel / padding, then the user application and a 16-byte MD5 trailer. `FWUPDATE` needs contiguous kernel / application data **without the MD5 trailer**. `USERUPDATE` needs the application including its 8-byte length / CRC header.

Run this in the firmware directory, using only a verified raw image, never a signed web package. It checks length, MD5, application magic, ABI, product and CRC before producing the two serial files:

```sh
python3 - <<'PY'
from pathlib import Path
import binascii
import hashlib
import struct

model = 'a1'
image = Path(f'phicomm_{model}.ota.bin.raw').read_bytes()
offset = 0x75000
assert offset + 144 <= len(image) <= offset + 0x40000 + 16, 'Invalid image length'
assert hashlib.md5(image[:-16]).digest() == image[-16:], 'OTA MD5 mismatch'
length, reserved, crc, magic, stack, abi = struct.unpack_from('<IHHIII', image, offset)
assert 120 <= length <= 0x40000 - 8, 'Invalid application length'
assert len(image) == offset + 8 + length + 16, 'Invalid application boundary'
assert magic == 0xc89346 and abi == 3, 'Application magic or ABI mismatch'
user = image[offset:-16]
assert reserved == crc == binascii.crc_hqx(user[8:], 0), 'Application CRC mismatch'
product = f'PHICOMM_{model.upper()}_EMW3080'.encode() + b'\0'
assert product in user, 'Image product does not match model'
Path('fw.serial.bin').write_bytes(image[:-16])
Path('user.serial.bin').write_bytes(user)
print('Kernel + application:', len(image) - 16, 'bytes')
print('User application:', len(user), 'bytes; SHA256:', hashlib.sha256(user).hexdigest())
PY
```

Record the resulting file lengths and compare them to the device's reported `Size`. Lengths and hashes vary by firmware; use your downloaded file's results.

### 3. Send and read back

1. At `MXCHIP>`, enter `1`. Wait for `Waiting for the file to be sent` / `C`, then send `fw.serial.bin` with **YMODEM CRC, 1 KiB packets**. Use a **50 ms packet interval** where supported; readback verification is still required.
2. Wait for `Successfully!` and check `Size`. This confirms only the bootloader's receive / write report, **not that the application can boot**.
3. Enter `2 -r` and use **Receive YMODEM** to save `USER.bin`. It must contain the complete `0x40000` user partition: **262144 bytes**. Exit minicom with **Ctrl-A, Q** before a Linux script takes exclusive access. Never let two programs read / write the serial port simultaneously.
4. Compare the application-length prefix against `user.serial.bin` and validate its CRC:

```sh
python3 - <<'PY'
from pathlib import Path
import binascii
import hashlib
import struct

expected = Path('user.serial.bin').read_bytes()
readback = Path('USER.bin').read_bytes()
assert len(readback) == 0x40000, 'User partition readback length mismatch'
actual = readback[:len(expected)]
assert actual == expected, 'Application readback mismatch; do not boot; write and read back again'
length, reserved, crc = struct.unpack_from('<IHH', actual)
assert length + 8 == len(actual), 'Application length mismatch'
assert reserved == crc == binascii.crc_hqx(actual[8:], 0), 'Application CRC mismatch'
print('Application bytes and CRC match; SHA256:', hashlib.sha256(actual).hexdigest())
PY
```

5. Only after matching readback, enter `6` to boot. Release BOOT from ground before a normal power cycle; restore original wiring as described in the preparation guide.

### 4. Recover an application CRC failure

`user firmware invalid [-3]` means the user application CRC failed.

1. Reenter the bootloader, verify checksums and regenerate the same firmware's user application file.
2. Enter `2` and send `user.serial.bin` via YMODEM with a **50 ms** packet interval. A matching product `.usr.bin` is also valid if verified identical to the extracted result.
3. After `Successfully!`, run `2 -r` and compare readback with the script above.
4. If bytes differ, check power, ground, wiring, exclusive port access and the USB-TTL adapter, then rewrite and read back. **Preserve the bootloader and parameters; never use whole-chip erase as trial and error.**
5. Enter `6` only when the application bytes match exactly.

### 5. Confirm application execution

Restore wiring and check the A1 setup hotspot, WebUI, fan and panel.

## Finish and provision Wi-Fi

> **Install the A1 filter before powering on.** An interlock switch at the filter can prevent the purifier from powering on when the filter is absent. If A1 appears unresponsive after flashing, first check that the filter is seated correctly and engages the switch. Source: [zA1 Issue #3](https://github.com/a2633063/zA1/issues/3).

1. Power off, release BOOT ground if used and remove programming wires.
2. Restore original wiring, reassemble, then reconnect the normal supply.
3. Join `A1-AP-<last six MAC digits>` and open `http://192.168.0.1/`.
4. Verify model and connectivity. Check the fan, ring and panel. Configure MQTT if needed and confirm connection.

See [A1 operation](a1-guide) and [firmware updates](firmware-update). Module wiring and serial procedures reference the [MXCHIP EMW3080 flashing guide](https://docs.mxchip.com/zh/foiwqd8gbgh23xu2/bwyo0fs5t2rquekz/antua5wkt8m3qx7l/lqdz2h5zpp117imt.html).
