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

Extract `daikin_G3_migration.tar.gz` and follow the [current bilingual instructions](https://github.com/phicomm-io/iotcloud-community-firmware/tree/main/tools/daikin_g3_migrate#readme). You need a Bluetooth-capable Linux host or Mac, a Docker service host reachable by the detector and DNS control restricted to this device. The republished RC6 includes both provisioners; Linux users with an older package can download `provision_linux.py` and `wifi.example.json` from the instructions directory. Linux uses BlueZ and the system Python's `python3-dbus`, and can run on a nearby host over SSH.

1. Prepare temporary services with the actual Wi-Fi STA MAC and service host IP, then configure the three per-device DNS overrides. Updates are initially disabled. The BLE address and virtual gateway MAC are separate identities.
2. Short-press the rear network button and send 2.4 GHz Wi-Fi settings plus a virtual gateway identity. Do not hold the reset button. Bluetooth closes after provisioning; short-press again before resending.
3. Run one gateway responder with `--empty-gateway --weather-code 1`. Wait for snapshot, weather, gateway login, filter and error-query acknowledgements. Stock MCU `SV.04.00.01.00` keeps retrying empty weather responses; the temporary icon code completes that prerequisite while temperature, humidity and PM2.5 remain missing.
4. Verify `daikin_BRY88BBA15G.ota.bin.raw`, explicitly enable the offer and send one ESP32-only update command.
5. Open the rebooted device page, check version, networking and readings, then remove the temporary DNS rules and services.

This first stage preserves the stock screen MCU and bootloader. It requires a working stock download path and does not use Daikin servers. You can configure [current weather](daikin-g3-services) with the stock MCU before proceeding to the community screen upgrade below.

### Online updates on community firmware

ESP32: open Settings → Firmware update, choose a source, check the version, read the changes and install. RC builds use a separate candidate channel. Installation verifies the model, signature, length and SHA-256. Custom raw applications use the explicitly labelled unsigned rescue upload.

The matching community screen temporarily puts the PM2.5 module to sleep during ESP32 OTA and resumes it after reboot. PM2.5 and TVOC readings may briefly be unavailable; confirm they refresh again after the update.

Screen MCU: open the device directly on the LAN and select Screen → Resources and updates. Both the first installation from stock screen firmware and later community screen updates support either method:

1. Online: click Check for updates, then Install screen update.
2. File: get the matching `daikin_G3_screen.bin` from [G3 downloads](../../en/index.html#daikin-g3-downloads), select it and click Upload and install.

The page automatically chooses factory installation or community staging/activation, verifies the firmware and completes installation. Wait for “Screen firmware installed and running”; this requires a new MCU session, a matching internal application digest and normal operation. Install the font separately online or by upload. Large screen uploads require a direct LAN connection.

This revision fixes intermittent PLL locking after handoff from the factory loader, which could leave the MCU at a low clock speed and enter SDRAM fault recovery. Line breaks in the update message no longer appear as question marks.

<a id="direct-uart-update"></a>

Update ESP32 to the matching RC6 first, then use either WebUI method above. The download package also includes a command-line migration tool:

The new tool and paired ESP32 can send the complete `daikin_G3_screen.bin` directly to stock MCU firmware. A recovery-first step, boot backup and stock-version allowlist are no longer required. The new MCU application also removes its local boot MD5 allowlist. Extract the paired tools, install Python `msgpack`, then run:

```sh
python3 -m pip install msgpack
python3 screen/flash_screen.py --url http://DEVICE_IP --image daikin_G3_screen.bin --mode install --log install.jsonl
```

The tool selects stock UART installation or community staging/activation automatically. Target, vectors, length, digests and staging readback are still checked. If an administration password is set, provide `IOTCLOUD_AUTH=admin:password`. Successful logs contain a new session and the running application's actual MD5.

Boot backup is optional. A community MCU can export the first 80 KiB twice with `screen/backup_screen_boot.py`; verify an existing pair using `--boot-backup DIRECTORY --reuse-boot-backup`. The original MCU has no export interface. The recovery image remains available for minimal-service diagnostics, where a black display is expected, but is no longer required for first installation.

The republished RC6 includes the new tool, ESP32 and MCU application. An older installed MCU application still enforces its own boot check until replaced; migration must satisfy that older version’s conditions. All screen updates preserve the stock bootloader. Keep stable power and use application images only for the matching loader copy contract.

The screen starts with minimal recovery services. By default, the ESP32 resumes startup after 5 seconds. Disable “Continue automatically after 5 seconds” in Resources & update to retain the complete 30-second window; the setting persists after power loss. Keep minimal services overrides automatic startup, and Start now ends a normal window immediately. Fault recovery never resumes automatically and requires repaired firmware. Communication and OTA start before the UI. OTA temporarily uses 10% brightness within the configured ceiling without changing saved settings. Application activation restarts the display.

Short-press the front key to change pages or wake the display. Hold it for about 3 seconds to shut down, then release it. When USB keeps the MCU powered, hold it again for about 2 seconds to turn it on; the shutdown press cannot immediately restart it. Screen OTA preserves the factory boot region.

## Optional high-resolution font

`daikin_G3_zh32.iotfnt` has 6863 glyphs at 32 px with 2 bpp grayscale. It is validated in external SPI NOR and cached in SDRAM. Uploading takes time; wait for the page to confirm the commit. Application updates do not rewrite the font. A nonblocking LVGL loading animation shows font progress and sensor readiness during startup. Paging remains available; missing sensors and failed resources leave the loading state after bounded waits. Startup finishes font loading and validation before enabling sensors to reduce concurrent load; the three sensor UARTs retain independent interrupt queues. After each screen startup, internal subset fonts appear first. The full font loads in the background and refreshes the display when ready; some Chinese characters in custom locations or titles may temporarily appear as placeholders.

NOR uses quad hardware SFC by default, with incremental font validation. The 1.64 MB font loaded in about 1.3 seconds on the tested device, including background scheduling and validation. Single-line SFC or GPIO requires an explicit ESP32 selection during diagnostic startup; hardware failures never switch the bus automatically. Unavailable NOR, corrupt data or read timeouts fall back to internal fonts and missing-glyph placeholders. Sensors, communication and recovery keep running. The screen application occupies about 198 KiB, leaving about 214 KiB in its 412 KiB partition.
