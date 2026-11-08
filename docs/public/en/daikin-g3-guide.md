# G3 installation, updates and recovery

[Layouts and appearance](daikin-g3-layout) · [Weather, reminders and API](daikin-g3-services) · [Startup and diagnostics](daikin-g3-diagnostics) · [Development](daikin-g3-development) · [Stock backups](stock-backups)

G3 is the BRY88BBA15G detector with a display. It has separate ESP32 networking and screen MCU firmware. The default name is `DAIKIN G3`; existing custom names are preserved.

## Identify the generation

| Generation | Model | HCHO expansion port | Display | Download group |
| --- | --- | --- | --- | --- |
| G1 | BRY88AA151K | None | None | Daikin air quality detector (G1/G2) |
| G2 | BRY88AB151K | Added compared with G1 | None | Daikin air quality detector (G1/G2) |
| G3 | BRY88BBA15G | Depends on the expansion module | Included | Daikin air quality detector (G3) |

See the [G1/G2 guide](daikin-guide) for installation and board selection. G1/G2 images cannot be used on G3.

## Download a matching release

Select the latest compatible G3 release from [Firmware & resources]({{SITE_URL}}en/#releases). Get its ESP32 image, screen MCU image and `daikin_G3_migration.tar.gz` together. The bundle includes Linux and macOS provisioners and optional screen installation scripts.

This guide uses the current RC6 release, paired with screen MCU `1.0.0-rc.6-55e52f`. Use the complete current tool bundle and firmware from the download page, or a newer compatible release. Check the release source revision and `SHA256SUMS`, and back up configuration.

## Wireless migration from stock firmware

Extract the current `daikin_G3_migration.tar.gz` and follow the [tool instructions](https://github.com/phicomm-io/iotcloud-community-firmware/tree/main/tools/daikin_g3_migrate#readme). You need a Bluetooth-equipped Linux host or Mac, a Docker service host reachable by the detector, and permission to configure DNS for this detector only. Linux uses BlueZ and the system Python `python3-dbus`; the nearby Bluetooth host can also be accessed over SSH.

1. Prepare the temporary service with the detector's actual Wi-Fi STA MAC and the service host IP. Configure the three documented domains. The service initially offers no update.
2. Short-press the rear network key and send the 2.4 GHz Wi-Fi and virtual gateway settings. Bluetooth closes after provisioning; short-press again to resend. A long press triggers reset.
3. Run one responder with `--empty-gateway --weather-code 1`. Wait for weather, gateway login, filter and error acknowledgements.
4. Verify `daikin_BRY88BBA15G.ota.bin.raw` from the same release, enable the offer and send one ESP32-only update command. Keep stable power connected during installation.
5. Open the device WebUI and check version, identity, connectivity and sensor readings. Remove the temporary DNS overrides and stop the services.

This stage preserves the stock screen MCU. The stock screen supports [current weather](daikin-g3-services); install the community screen MCU for layouts, themes and reminders.

## Online updates on community firmware

### ESP32

Open **Settings → Firmware**, check for the latest compatible version and install it. RC builds use a separate candidate channel. Online updates check model, signature, length and SHA-256. The unsigned rescue entry accepts self-built raw applications.

The paired community screen pauses the PM2.5/TVOC module during ESP32 OTA and resumes it after reboot. Check that both readings refresh after the update.

### Screen MCU: online or file installation

Update the paired ESP32 first. Open the detector directly on the LAN and go to **Settings → Screen → Resources and update**. First installation from stock and later updates share this entry. Choose either:

1. Online: click **Check updates**, then **Install screen update**.
2. File: download `daikin_G3_screen.bin` from the same release, select it and click **Upload and install**.

Keep power connected and wait for **Screen firmware installed and running normally**. Installation verifies, transfers and activates the application while preserving the stock boot region. First installation needs no preliminary boot backup or recovery image. Check the screen version, pages and sensor readings afterward. The font can be installed separately.

<a id="direct-uart-update"></a>

For command-line installation, use the current migration bundle. Extract it, install Python `msgpack`, then run:

```sh
python3 -m pip install msgpack
python3 screen/flash_screen.py --url http://DEVICE_IP --image daikin_G3_screen.bin --mode install --log install.jsonl
```

If a management password is configured, supply `IOTCLOUD_AUTH=admin:PASSWORD` in the environment. Optional boot backup and recovery image instructions are in [Development](daikin-g3-development#installation-and-recovery).

## Settings and keys

- Layout: compose clocks, readings, text, charts, weather and reminders; drag, resize, undo and keep local drafts.
- Appearance: themes, widget colors, wallpaper and opacity.
- Display: brightness, sleep, weekly dimming, sleep page and Chinese/English.
- Weather and reminders: providers, locations, schedules and API pushes.
- Resources and update: fonts, screen MCU updates and the startup maintenance window.

Short-press the front key to change pages; the first short press from sleep wakes the screen. Hold about 3 seconds to shut down. Release, then hold about 2 seconds to start again. The rear network key is for provisioning.

The screen opens a maintenance window at startup and continues automatically after 5 seconds. Disable **Automatically continue after 5 seconds** for the full 30-second window, or choose **Keep minimal services** or **Continue startup now**. Fault maintenance requires a repaired image; see [Startup and diagnostics](daikin-g3-diagnostics).

## Optional high-resolution font

`daikin_G3_zh32.iotfnt` provides 6863 Chinese glyphs at 32 px. Install online or upload it in **Resources and update**, then wait for completion. Ordinary MCU updates preserve the font.

Startup initially uses the built-in compact font while the full font loads in the background. Custom Chinese characters may briefly appear as placeholders. Check resource status and progress; the screen refreshes when loading completes. If the resource is unavailable, the built-in font is used while sensors and update services continue.
