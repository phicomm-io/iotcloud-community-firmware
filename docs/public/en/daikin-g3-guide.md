# G3 installation, updates and recovery

[Screen layouts and appearance](daikin-g3-layout) · [Weather, reminders and API](daikin-g3-services) · [Startup and diagnostics](daikin-g3-diagnostics) · [Development resources](daikin-g3-development) · [Stock backups](stock-backups)

G3 is the BRY88BBA15G with a display, an ESP32 network application and a separate screen MCU application. Its default device name is `DAIKIN G3`. Custom names are preserved.

## Identify the generation

| Generation | Model | Formaldehyde expansion port | Display | Download group |
| --- | --- | --- | --- | --- |
| G1 | BRY88AA151K | None | None | Daikin air quality monitor (1st/2nd generation) |
| G2 | BRY88AB151K | Added compared with G1 | None | Daikin air quality monitor (1st/2nd generation) |
| G3 | BRY88BBA15G | Depends on the installed expansion module | Added display | Daikin air quality monitor (3rd generation) |

G1/G2 defaults to `DAIKIN G1/G2`. Generation labels identify the product; check the board before selecting firmware. The early BRY88AA151K has different pins and TVOC hardware from the universal BRY88AX151K target. A G1 label alone does not establish compatibility. See the [G1/G2 guide](daikin-guide) for supported boards and installation.

## Screen settings

Open Settings → Screen on the device website:

- Layout: combine clocks, values, text, charts, weather and reminders; drag, resize, undo and retain local drafts. Hide a full-page component's heading to reclaim space.
- Appearance: six themes, custom palettes, independent component colors, wallpaper and opacity.
- Display: brightness, sleep timeout, weekly dimming plans, idle page and Chinese/English. Device names and custom titles are not translated automatically.
- Weather & reminders: configure a provider, location, schedules or API notices. Open-Meteo needs no API key. TLS certificate verification defaults to off and can be enabled.
- Resources & update: optional high-resolution Chinese font, screen UART OTA, and startup controls to start now or keep minimal services.
- Advanced: AM1008L SET (PD12) control and diagnostics.

The screen uses a full-width page rail, 16 px outer margins and 8 px component gaps. The web preview uses the same geometry. Under **Screen → Appearance**, choose a 6, 12 or 20 px rail; the default is 12 px.

Local charts offer 32, 64, 128 or 256 points. Samples are collected every minute; shorter histories fill the plot and missing data stays a gap. MCU resets restart history collection. External API series retain the 32-point-per-series protocol.

## Downloads and updates

[Firmware & resources]({{SITE_URL}}en/#releases) offers the latest compatible G3 releases, including signed ESP32 updates, a raw application for stock migration, the screen MCU application, an optional font and wireless migration tools. G1/G2 images are incompatible with G3.

### Wireless migration from stock firmware

Extract `daikin_G3_migration.tar.gz` and follow its [bilingual instructions](https://github.com/phicomm-io/iotcloud-community-firmware/tree/main/tools/daikin_g3_migrate#readme). You need a Bluetooth-capable Mac, a Docker host on the same LAN and DNS control restricted to this device.

1. Prepare temporary services with the device MAC and the host LAN IP. Updates are initially disabled.
2. Short-press the rear network button and send Wi-Fi settings plus a virtual gateway identity. Do not hold the reset button.
3. Redirect only the three documented domains for this device and check association and version-query logs.
4. Verify `daikin_BRY88BBA15G.ota.bin.raw`, explicitly enable the offer and send one ESP32-only update command.
5. Open the rebooted device page, check version, networking and readings, then remove the temporary DNS rules and services.

This first stage preserves the stock screen MCU and bootloader. It requires a working stock download path and does not use Daikin servers.

### Online updates on community firmware

ESP32: open Settings → Firmware update, choose a source, check the version, read the changes and install. RC builds use a separate candidate channel. Installation verifies the model, signature, length and SHA-256. Custom raw applications use the explicitly labelled unsigned rescue upload.

Screen and font: open the device directly on the LAN and select Screen → Resources and updates → Online updates. Check available versions and install the application or font separately. The browser checks file size; ESP32 verifies the digest and staging before MCU installation. Cloud access supports controls and ESP32 updates; large screen uploads currently require a direct LAN connection.

First migration from the stock screen requires two stages. Download `daikin_G3_recovery.bin` for minimal recovery services; a black display is expected after installation. `daikin_G3_screen.bin` provides the complete UI. The stock MCU must report `SV.04.00.01.00`, and the ESP32 must already run community firmware. Extract the migration tools, install Python `msgpack`, then run these commands from the tools directory, replacing the device address:

```sh
python3 -m pip install msgpack
python3 screen/flash_screen.py --url http://DEVICE_IP --image ../daikin_G3_recovery.bin --mode factory-bootstrap --log bootstrap.jsonl
python3 screen/backup_screen_boot.py --url http://DEVICE_IP --output boot-backup --hold
python3 screen/flash_screen.py --url http://DEVICE_IP --image ../daikin_G3_screen.bin --mode install --boot-backup boot-backup --log install.jsonl
```

Retain any existing stock backup. Once the recovery image runs, the backup tool reads the first 80 KiB twice. Full installation requires matching contents and checksums. If an administration password is set, provide it through `IOTCLOUD_AUTH=admin:password`. Logs verify the running image MD5. Reuse an unchanged bootloader backup by adding `--reuse-boot-backup` to later installation commands; the web UI can also save it. Application images are not full-chip dumps; screen OTA preserves the existing bootloader.

The screen starts with minimal recovery services. By default, the ESP32 resumes startup after 5 seconds. Disable “Continue automatically after 5 seconds” in Resources & update to retain the complete 30-second window; the setting persists after power loss. Keep minimal services overrides automatic startup, and Start now ends a normal window immediately. Fault recovery never resumes automatically and requires repaired firmware. Communication and OTA start before the UI. OTA temporarily uses 10% brightness within the configured ceiling without changing saved settings. Application activation restarts the display. The factory bootloader's power-on gate is retained, so a short front-key press may be needed after complete power loss. Automatic startup has not been implemented by modifying the bootloader.

## Optional high-resolution font

`daikin_G3_zh32.iotfnt` has 6863 glyphs at 32 px with 2 bpp grayscale. It is validated in external SPI NOR and cached in SDRAM. Uploading takes time; wait for the page to confirm the commit. Application updates do not rewrite the font. A nonblocking LVGL loading animation shows font progress and sensor readiness during startup. Paging remains available; missing sensors and failed resources leave the loading state after bounded waits. Three independent sensor UART interrupt queues preserve readings while the font loads. After each screen startup, internal subset fonts appear first. The full font loads in the background and refreshes the display when ready; some Chinese characters in custom locations or titles may temporarily appear as placeholders.

Fonts use hardware SFC reads with incremental validation. The 1.64 MB font loaded in about 14 seconds on the tested device, including background scheduling and validation. A failed hardware comparison or controller error falls back to GPIO. Unavailable NOR, corrupt data or read timeouts then fall back to internal fonts and missing-glyph placeholders. Sensors, communication and recovery keep running. The screen application occupies about 196 KiB, leaving about 216 KiB in its 412 KiB partition.
