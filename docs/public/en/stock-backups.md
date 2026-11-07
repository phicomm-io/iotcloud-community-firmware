# Stock backups and recovery

The [stock-backup section]({{SITE_URL}}en/#stock-backups) groups programs, controllers and resources by device. Every download includes its size and SHA-256. The [machine-readable catalog](assets/firmware/stock/manifest.json) also records the uncompressed size, checksum, format and confirmed Flash address.

Published files contain shareable programs and resources. Regions containing Wi-Fi credentials, accounts, factory parameters or calibration remain in private backups. Keep a full backup of your own device before installation. Choose current community firmware under [Firmware & resources]({{SITE_URL}}en/#releases); stock files retain their original versions.

## Available devices

| Device | Component and version | Scope and recovery conditions |
| --- | --- | --- |
| TC1 A1 | Stock distribution 1.0.0.21, gzip compressed | 2 MiB uncompressed; retained distribution format, direct restoration not validated; not community OTA |
| M1 Wi-Fi module | Stock 14.2.4.1 kernel and application | `0x13000..0xC8000`, 741376 bytes; requires the matching stock serial Bootloader MEMORYMAP |
| M1 sensor controller | ATSAMD20G18A 11.1.2.1 | 256 KiB main Flash including boot and application; excludes User Row / fuses; not application OTA |
| A1 Wi-Fi module | Stock 1.47 kernel and application | `0x13000..0xC8000`, 741376 bytes; preserve local boot and parameters; check the A1 partition map |
| S30-V1B / S30M_V1B | Stock 2M.1220 application | Physical `0x11000..0x132000`, including BK7231N CRC bytes; UART recovery with the retained stock bootloader only |
| Daikin G2 BRY88AB151K | SensorKit2 application, ESP32 bootloader and partition table | Extracted from the original UART dump; application metadata: `SensorKit2 / 1 / 2020-01-16` |
| Daikin G3 BRY88BBA15G | ESP32 V01.02.00 application, bootloader and partition table | Extracted from two matching 8 MiB reads, excluding NVS, OTA selection and device configuration |
| Daikin G3 screen MCU | Vendor OTA application SV.04.00.04.00 | 421888 bytes, application base `0x14000`; not a complete dump of the device's initial SV.04.00.01.00 |
| Daikin G3 screen MCU | Original first 80 KiB boot region | Two matching read-only backups; for comparison and offline recovery research; ordinary serial OTA preserves it |
| Daikin G3 external SPI NOR | Stock screen resources | Decompresses to a full 16 MiB NOR image with animation, pictures and glyphs; not an MCU application |

G1 is BRY88AA151K, G2 is BRY88AB151K and G3 is the screen-equipped BRY88BBA15G. No verified stock G1 image is available locally; G2 files cannot substitute for it. `BRY88AX151K` is the wildcard name for AA/AB; see the [G1/G2 guide](daikin-guide) for community firmware.

## Download and verify

Expand SHA-256 below a download or use [SHA256SUMS](assets/firmware/stock/SHA256SUMS). This list covers files in `stock/`; the M1 controller reuses its [existing file and checksum documentation](m1-mcu-firmware#stock-controller-backup).

On Linux, run `sha256sum -c SHA256SUMS --ignore-missing` in the download directory. On macOS use `shasum -a 256 filename`; in Windows PowerShell use `Get-FileHash filename -Algorithm SHA256`. Verify `.gz` files before decompressing them to `.bin`; the catalog includes checksums for the resulting images.

The catalog's `address` is a physical Flash offset. It does not replace a recovery tool's partition number. MiCO's serial `FWUPDATE` menu and partition-write interfaces use their own selectors.

## M1, A1 and S30

The M1 Wi-Fi recovery file came from the original backup. Historical restoration verified every destination byte, unchanged bytes outside the destination and a running stock application. Follow [M1 wired recovery](m1-flash), open the stock menu and check that `MEMORYMAP` lists MiCO Kernel at `0x13000` with length `0xB5000`. Only with a matching map, use `FWUPDATE` YMODEM binary transfer and readback. Preserve the bootloader, parameters and local factory data.

The A1 1.47 file also contains only kernel and application. Check the actual map using [A1 wired recovery](a1-flash). The M1 sensor controller is a separate chip: restoring its full main Flash needs a debug interface; see the [controller guide](m1-mcu-firmware).

The S30 file already includes physical Flash CRC bytes. It differs from a logical application with CRC removed, RBL and UG. Follow [S30 serial installation](s30-flash#disassembly-and-uart) using a tool that accepts physical CRC images; do not add CRC again. This archive does not provide a stock-restoration RBL network package.

The TC1 archive retains the vendor distribution format, without restoration validation in this run. Preserve a full device read and use [TC1 wired recovery](tc1-flash) to identify the module, layout and tool input format. A 2 MiB file size alone does not establish its write address.

## Daikin ESP32 and screen

The original BRY88AB151K 4 MiB UART dump matches its restoration readback: SHA-256 `f8f318b8b85c98b7599706a7e759df684a4ed4efbb81fc7f171b46fe347c4aac`. Public application, bootloader and partition-table files are extracted from it. ESP32 image checksums and appended hashes are verified; remaining regions are not published.

Both G2 and G3 ESP32 component addresses are bootloader `0x1000`, partition table `0x8000` and application `0x10000`. Use a matching model and partition set. Ordinary application recovery should preserve the device's bootloader, NVS, PHY and OTA-selection data; devices with two OTA slots also need their selected slot checked. These component files are not a one-click full restore. Community signed-upload interfaces do not automatically accept stock applications.

G3 screen application, boot region and external NOR are three different files. Normal screen recovery uses the [serial OTA channel](daikin-g3-guide#online-updates-on-community-firmware), preserving the original bootloader. Stock NOR resources and community font/wallpaper storage use different layouts; do not overwrite the entire NOR during operation. The vendor SV.04.00.04.00 screen application paired with ESP32 V01.02.00 has not been tested on hardware in this run. Preserve the boot region when SWD recovery is unavailable.

## Keep your own backup

Retain complete reads, a second readback, SHA-256, device model, wiring/power details and tool versions. Share program components while preserving your device's NVS, RF calibration, factory parameters, keys and sensor calibration locally. After restoration, check running versions, networking, readings, buttons, display and charging separately; a successful write alone does not confirm operation.
