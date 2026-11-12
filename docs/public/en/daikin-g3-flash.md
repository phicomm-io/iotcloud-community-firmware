# G3 wireless, serial and screen flashing

For the **BRY88BBA15G / SENSOR3_ESP32** with a display. G3 has separate ESP32 networking and SWM34S screen MCU firmware. See [G3 features and controls](daikin-g3-guide) for daily use and [Flashing preparation](flash-prep) before opening the device.

**Install ESP32 wirelessly first, then install the screen MCU through the WebUI.** If community ESP32 firmware is already running, start at screen installation.

<!-- os-picker -->

## Choose a flashing method

| Current state | Method | Tools |
| --- | --- | --- |
| Stock firmware boots and connects to Wi-Fi | [Wireless ESP32 installation](#wireless-esp32-installation) | Bluetooth Linux host / Mac, Docker, per-device DNS |
| ESP32 cannot connect, or needs a full backup or recovery | [Serial ESP32 installation](#serial-esp32-installation) | 3.3V USB-TTL, stable 3.3V supply, Python |
| Community ESP32 is online and the stock or community screen responds | [Screen MCU installation through WebUI](#screen-mcu-installation-through-webui) | Windows / Linux / macOS browser |
| Screen MCU does not communicate and needs wired recovery | [Screen MCU installation with J-Link](#screen-mcu-installation-with-j-link) | STAR-MC1-compatible J-Link, Synwit plugin, stable 3.3V supply |

Windows runs the wireless process over SSH on a Bluetooth Linux host on the same LAN. Linux and macOS can provision directly. Serial and J-Link installation are available on all three systems.

## Download files

Select **Daikin air quality detector (G3)** in [Firmware & resources]({{SITE_URL}}en/#releases). This guide uses the current **v1.0.0-rc.6**. A newer compatible release is also suitable; get tools and firmware from the same release.

| Purpose | File | Installation |
| --- | --- | --- |
| Wireless ESP32 installation from stock | `daikin_BRY88BBA15G.ota.bin.raw` | Temporary OTA service |
| Serial ESP32 installation | `daikin_BRY88BBA15G.all.bin` | Full image at `0x0` |
| ESP32 updates on community firmware | `daikin_BRY88BBA15G.ota.bin.signed` | Settings → Firmware |
| Screen MCU first installation, update and application recovery | `daikin_G3_screen.bin` | WebUI, or J-Link at `0x14000` |
| Wireless migration tools | `daikin_G3_migration.tar.gz` | Extract before use |
| Optional high-resolution font | `daikin_G3_zh32.iotfnt` | Screen resource page |
| Checksums | `SHA256SUMS` | Compare downloaded SHA-256 values |

Download the required files and `SHA256SUMS` from the [current RC6 release](https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/v1.0.0-rc.6). Check the files used for your method:

<!-- os:windows -->
### Windows: verify downloads

Open PowerShell in the download folder. For example, check the serial image:

```powershell
Get-FileHash .\daikin_BRY88BBA15G.all.bin -Algorithm SHA256
Select-String -Path .\SHA256SUMS -Pattern '  daikin_BRY88BBA15G.all.bin$'
```

<!-- /os -->
<!-- os:linux -->
### Linux: verify downloads

Run in the download folder. For example, check the serial image:

```sh
sha256sum daikin_BRY88BBA15G.all.bin
grep '  daikin_BRY88BBA15G.all.bin$' SHA256SUMS
```

<!-- /os -->
<!-- os:mac -->
### macOS: verify downloads

Run in the download folder. For example, check the serial image:

```sh
shasum -a 256 daikin_BRY88BBA15G.all.bin
grep '  daikin_BRY88BBA15G.all.bin$' SHA256SUMS
```

<!-- /os -->

Both digests must match; letter case does not matter. Substitute the appropriate filename for the tool bundle, raw application or screen image. G1/G2 `BRY88AX151K` images cannot be used on G3.

## Wireless ESP32 installation

### 1. Prepare tools and temporary services

Keep USB power connected and prepare a 2.4 GHz Wi-Fi network. Replace `192.168.1.10` with the service host IP and `AABBCCDDEEFF` with the detector's actual **Wi-Fi STA MAC**. Obtain the STA MAC from the router's DHCP records or device label; record its Bluetooth address separately.

The service host needs Python 3, OpenSSL and Docker Compose, with LAN TCP **443 / 8883** reachable from the detector. The router must allow DNS settings for this device. Place the Bluetooth host nearby; it can also host the services.

<!-- os:windows -->
### Windows: use a Linux host

Prepare a Bluetooth-equipped Debian / Ubuntu host on the same LAN. Install Engine and its Compose plugin following [Docker's instructions](https://docs.docker.com/engine/install/). In Windows PowerShell, copy the verified bundle and raw application, then log in:

```powershell
ssh USER@192.168.1.10 'mkdir -p ~/g3-flash'
scp .\daikin_G3_migration.tar.gz .\daikin_BRY88BBA15G.ota.bin.raw USER@192.168.1.10:g3-flash/
ssh USER@192.168.1.10
```

Run all remaining wireless commands in that SSH session:

```sh
sudo apt update
sudo apt install python3 openssl bluez python3-dbus
cd ~/g3-flash
tar -xzf daikin_G3_migration.tar.gz
cd daikin-g3-migrate
cp ../daikin_BRY88BBA15G.ota.bin.raw .
```

<!-- /os -->
<!-- os:linux -->
### Linux: install and extract

On Debian / Ubuntu, install the dependencies and set up Engine with its Compose plugin following [Docker's instructions](https://docs.docker.com/engine/install/). Run from the download folder:

```sh
sudo apt update
sudo apt install python3 openssl bluez python3-dbus
tar -xzf daikin_G3_migration.tar.gz
cd daikin-g3-migrate
cp ../daikin_BRY88BBA15G.ota.bin.raw .
```

<!-- /os -->
<!-- os:mac -->
### macOS: install and extract

Install Python 3, OpenSSL and [Docker Desktop](https://docs.docker.com/desktop/setup/install/mac-install/), then start Docker Desktop. Install Xcode Command Line Tools and extract the bundle in the download folder:

```sh
xcode-select --install
tar -xzf daikin_G3_migration.tar.gz
cd daikin-g3-migrate
cp ../daikin_BRY88BBA15G.ota.bin.raw .
```

Allow the provisioning app to use Bluetooth. Host the temporary services locally or on a Linux host on the same LAN. With a remote service host, run service creation, responder and update commands there; only provisioning runs on the Mac.

<!-- /os -->

Check that your user can run `docker info` and `docker compose version`. On Linux, configure Docker permissions using the [post-installation instructions](https://docs.docker.com/engine/install/linux-postinstall/). Then create services from the tool directory as your regular user:

```sh
python3 init.py --address 192.168.1.10 --device AABBCCDDEEFF --output ./lab
docker compose -f ./lab/compose.json up -d
```

### 2. Configure device DNS and Bluetooth

For this detector, resolve the following domains to `192.168.1.10` and set its DHCP DNS to that resolver. Restart the detector normally or renew its DHCP lease:

```text
newlifemulti.daikin-china.com.cn
newlifemulti-cdh.daikin-china.com.cn
newlifemulti-cdh-core.daikin-china.com.cn
```

Short-press the rear network key and locate `SENSOR3_…`. Use virtual gateway MAC `020000000001` and the service host IP as gateway IP.

<!-- os:windows linux -->
### Windows / Linux: BlueZ provisioning

On the Bluetooth Linux host, run `bluetoothctl`, enter `power on` and `scan on`, and record the selected device name and Bluetooth address. Enter `scan off`, then `quit`. Create a private configuration in the tool directory:

```sh
cp wifi.example.json wifi.private.json
chmod 600 wifi.private.json
nano wifi.private.json
```

Fill in the actual values, for example:

```json
{
  "address": "AA:BB:CC:11:22:33",
  "device_name": "SENSOR3_AABBCC",
  "ssid": "YOUR_2G_WIFI",
  "password": "YOUR_WIFI_PASSWORD",
  "gateway_ip": "192.168.1.10",
  "gateway_mac": "020000000001"
}
```

Save, then run:

```sh
/usr/bin/python3 provision_linux.py < wifi.private.json
```

<!-- /os -->
<!-- os:mac -->
### macOS: CoreBluetooth provisioning

Copy the configuration and fill in the Bluetooth device name, 2.4 GHz Wi-Fi credentials and service host `gateway_ip`. Keep the example virtual gateway MAC:

```sh
cp wifi.example.plist wifi.private.plist
chmod 600 wifi.private.plist
open -e wifi.private.plist
./build_macos.sh
./IotcloudG3Provision.app/Contents/MacOS/provision "$PWD/wifi.private.plist"
```

Accept the macOS Bluetooth permission prompt. The tool sends the stock firmware's Wi-Fi and virtual gateway configuration.

<!-- /os -->

Bluetooth closes after provisioning. Short-press the rear network key again to resend; holding that key triggers reset.

### 3. Run acknowledgements and install ESP32

Open another terminal on the service host, enter the same tool directory and run one responder:

```sh
python3 local_gateway_responder.py --device AABBCCDDEEFF \
  --gateway 020000000001 --host 192.168.1.10 \
  --empty-gateway --weather-code 1 --seconds 600
```

Wait until the log confirms acknowledgements for `snapshot_upload`, `get_weather`, `login_ipbox`, `get_filter_sensor3` and `get_error_sensor3`. Then enable the update in another terminal:

```sh
python3 enable.py --directory ./lab --device AABBCCDDEEFF \
  --firmware ./daikin_BRY88BBA15G.ota.bin.raw --version V99.00.00
docker compose -f ./lab/compose.json up -d --no-deps --force-recreate https
python3 trigger.py --device AABBCCDDEEFF --host 192.168.1.10 \
  --version V99.00.00 --trigger-esp32
docker compose -f ./lab/compose.json logs -f https
```

`V99.00.00` is the stock OTA target label for this operation; the community application reports its actual RC6 version after reboot. Wait for an `artifact` HTTP 200 log with the full transfer length, followed by automatic restart. This step updates ESP32 while preserving the stock screen.

### 4. Verify and clean up

Find the detector IP in the router and open `http://DEVICE_IP/`. Check the G3 model, version, Wi-Fi and sensor readings. The stock screen supports current weather temperature, humidity and icons; install the community screen below for layouts and themes.

After verification, remove this device's three DNS overrides, restore its DHCP DNS, stop the responder and shut down temporary services:

```sh
docker compose -f ./lab/compose.json down
```

Remove the temporary `lab` and filled Wi-Fi configuration files. If there is no download request, check device DNS, the virtual gateway and acknowledgement logs. See the [migration tool README](https://github.com/phicomm-io/iotcloud-community-firmware/tree/main/tools/daikin_g3_migrate#readme) for tool details.

## Serial ESP32 installation

### 1. Wiring and download mode

Disconnect USB and the battery before wiring. Use a stable, separate **3.3V supply** with common ground to the USB-TTL adapter. The adapter must use 3.3V logic. The device USB power socket is separate from its download UART.

| ESP32 signal | Connection |
| --- | --- |
| 3V3 | Separate 3.3V supply |
| GND | Supply negative and USB-TTL GND |
| GPIO1 / U0TXD | USB-TTL RX |
| GPIO3 / U0RXD | USB-TTL TX |
| GPIO0 | GND during download; release afterward |
| EN / CHIP_PU | Briefly pull low, then release to reset ESP32 |

GPIO16 / GPIO17 carry ESP32-to-screen traffic. Flash using **GPIO1 / GPIO3** above. Following [Espressif's download-mode instructions](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html), hold GPIO0 low during reset or power-on to enter ROM download mode. Repeat this when a command needs a fresh handshake.

### 2. Install tools and back up

Install Python **3.10 or newer** and close other serial applications. These examples use esptool 5; replace the example port. `--before no-reset --after no-reset` is used with manual reset.

<!-- os:windows -->
### Windows: PowerShell

Find the adapter COM number under **Ports** in Device Manager. This example uses `COM5`:

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install "esptool>=5,<6"
.\.venv\Scripts\python.exe -m esptool --chip esp32 --port COM5 --baud 115200 --before no-reset --after no-reset read-flash 0 0x800000 g3-stock-1.bin
.\.venv\Scripts\python.exe -m esptool --chip esp32 --port COM5 --baud 115200 --before no-reset --after no-reset read-flash 0 0x800000 g3-stock-2.bin
Get-FileHash .\g3-stock-1.bin, .\g3-stock-2.bin -Algorithm SHA256
```

<!-- /os -->
<!-- os:linux -->
### Linux: terminal

Install `python3-venv`, add your user to the serial device's group (commonly `dialout`), and log in again. This example uses `/dev/ttyUSB0`:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install 'esptool>=5,<6'
python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --before no-reset --after no-reset read-flash 0 0x800000 g3-stock-1.bin
python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --before no-reset --after no-reset read-flash 0 0x800000 g3-stock-2.bin
sha256sum g3-stock-1.bin g3-stock-2.bin
```

<!-- /os -->
<!-- os:mac -->
### macOS: terminal

Find the adapter with `ls /dev/cu.*`. This example uses `/dev/cu.usbserial-0001`:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install 'esptool>=5,<6'
python -m esptool --chip esp32 --port /dev/cu.usbserial-0001 --baud 115200 --before no-reset --after no-reset read-flash 0 0x800000 g3-stock-1.bin
python -m esptool --chip esp32 --port /dev/cu.usbserial-0001 --baud 115200 --before no-reset --after no-reset read-flash 0 0x800000 g3-stock-2.bin
shasum -a 256 g3-stock-1.bin g3-stock-2.bin
```

<!-- /os -->

Both files must contain **8388608 bytes** and have matching SHA-256 digests. Keep the backups and checksums privately before writing.

### 3. Write the full image

Verify `daikin_BRY88BBA15G.all.bin` and write it at **`0x0`**. The full image replaces ESP32 boot code, partition table and application; check Wi-Fi and other settings afterward. A separate full-chip erase is unnecessary.

<!-- os:windows -->
```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32 --port COM5 --baud 115200 --before no-reset --after no-reset write-flash 0x0 daikin_BRY88BBA15G.all.bin
.\.venv\Scripts\python.exe -m esptool --chip esp32 --port COM5 --baud 115200 --before no-reset --after no-reset verify-flash 0x0 daikin_BRY88BBA15G.all.bin
```
<!-- /os -->
<!-- os:linux -->
```sh
python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --before no-reset --after no-reset write-flash 0x0 daikin_BRY88BBA15G.all.bin
python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 --before no-reset --after no-reset verify-flash 0x0 daikin_BRY88BBA15G.all.bin
```
<!-- /os -->
<!-- os:mac -->
```sh
python -m esptool --chip esp32 --port /dev/cu.usbserial-0001 --baud 115200 --before no-reset --after no-reset write-flash 0x0 daikin_BRY88BBA15G.all.bin
python -m esptool --chip esp32 --port /dev/cu.usbserial-0001 --baud 115200 --before no-reset --after no-reset verify-flash 0x0 daikin_BRY88BBA15G.all.bin
```
<!-- /os -->

After successful write and verification, power off, release GPIO0 and remove programming wires before restoring the battery and USB. Hold the front key about 2 seconds to start. If Wi-Fi does not reconnect, follow [Wi-Fi setup](quick-start), connect to the community hotspot and open **http://192.168.0.1**. Check the G3 model, version and sensor readings.

## Screen MCU installation through WebUI

Windows, Linux and macOS share these steps. Install the paired community ESP32 first, keep USB power connected and **open the device IP directly** in a LAN browser. Go to **Settings → Screen → Resources and update**. First installation from stock and later community updates use the same entry.

| Method | Steps |
| --- | --- |
| Online | Select an update source, then **Check updates → Install screen update** |
| Local file | Verify `daikin_G3_screen.bin` from the same release, select it, then **Upload and install** |

Wait for **Screen firmware installed and running normally**, then check the MCU version, pages and sensor readings. Installation transfers, verifies and activates the application while preserving the stock boot region. No preliminary `recovery.bin` installation or boot export is needed. Both first installation and updates use the normal `daikin_G3_screen.bin`.

Install the optional `daikin_G3_zh32.iotfnt` online or by upload on the same resource page. Normal MCU updates preserve installed fonts. Later ESP32 updates use **Settings → Firmware** and the corresponding signed package.

## Screen MCU installation with J-Link

### 1. Tools and wiring

Use a **STAR-MC1-compatible** J-Link. [Synwit's requirements](https://www.synwit.cn/servicesupport/447.html) specify J-Link hardware V9 or newer. Install [SEGGER J-Link software](https://www.segger.com/downloads/jlink/) for your OS and the Synwit flash plugin below.

Disconnect USB and the battery before wiring. Use a stable, separate 3.3V supply and power on after wiring is complete. J-Link VTref senses target voltage; it does not power the board.

| Screen MCU signal | J-Link signal | Standard 20-pin connector |
| --- | --- | --- |
| PB14 / SWDIO | SWDIO | 7 |
| PB12 / SWCLK | SWCLK | 9 |
| P50 / nRESET test point | nRESET | 15 |
| 3V3 | VTref and separate 3.3V supply | 1 |
| GND | GND and supply negative | 4 |

P50 is a board test-point name. Use the signal labels on compact probes; the pin numbers apply to the standard connector below.

![Standard J-Link SWD connector](assets/images/jlink-swd.png)

### 2. Install the Synwit plugin

Download and extract the [chip vendor's JFlash plugin](https://github.com/Synwit-Co-Ltd/JFlash/archive/7dc3a294fbfc23b7e533006ad301354f597777a6.zip). With current J-Link software, arrange its `JLinkDevices.xml` and `Synwit` folder as follows:

```text
JLinkDevices/
  Synwit/
    JLinkDevices.xml
    Devices/
      Synwit/
        SWM341xE.FLM
        ...
```

<!-- os:windows -->
### Windows: plugin folder and launch

The plugin root is `%APPDATA%\SEGGER\JLinkDevices`. In PowerShell, add your J-Link installation folder to this session's PATH, for example:

```powershell
$env:Path = 'C:\Program Files\SEGGER\JLink;' + $env:Path
JLink.exe -Device SWM341xE -If SWD -Speed 100 -AutoConnect 1
```

<!-- /os -->
<!-- os:linux -->
### Linux: plugin folder and launch

The plugin root is `~/.config/SEGGER/JLinkDevices`. After installing SEGGER software and its USB rules, run from the firmware folder:

```sh
JLinkExe -Device SWM341xE -If SWD -Speed 100 -AutoConnect 1
```

<!-- /os -->
<!-- os:mac -->
### macOS: plugin folder and launch

The plugin root is `~/Library/Application Support/SEGGER/JLinkDevices`. Run from the firmware folder, replacing the application path with your installed location:

```sh
/Applications/SEGGER/JLink/JLinkExe -Device SWM341xE -If SWD -Speed 100 -AutoConnect 1
```

<!-- /os -->

With multiple probes attached, add `-USB PROBE_SERIAL_NUMBER`. Confirm `SWM341xE`, an established SWD connection and VTref near 3.3V before continuing.

### 3. Back up, then write the application

In J-Link Commander, halt the MCU and save two copies of the 512 KiB internal Flash:

```text
h
savebin g3-mcu-stock-1.bin 0x0 0x80000
savebin g3-mcu-stock-2.bin 0x0 0x80000
```

In another terminal, compare the copies with this page's SHA-256 commands. Both must contain **524288 bytes** and have matching digests. Keep these backups, which include this device's boot code and configuration. Verify the downloaded `daikin_G3_screen.bin`, then return to J-Link Commander:

```text
loadfile daikin_G3_screen.bin 0x14000 noreset
verifybin daikin_G3_screen.bin 0x14000
savebin g3-mcu-installed.bin 0x14000 0x67000
```

The current RC6 application contains **421888 bytes**, covering `0x14000` through `0x7afff`. The readback SHA-256 must match the downloaded image. **Do not run a full-chip erase or write the application at `0x0`.** Preserve the first 80 KiB boot region and the configuration region starting at `0x7e000`.

After verification, enter `r`, `g`, then `exit`. Power off, remove debugging wires and restore normal power. Hold the front key about 2 seconds to start. Open the WebUI and check the MCU version and readings. Install the optional high-resolution font separately through the WebUI.

For `Unknown device`, check the plugin folder. For `Specific core setup failed`, check J-Link hardware and STAR-MC1 support. Application recovery requires an intact boot region; restore a damaged boot region from this device's backup following [Stock backups and recovery](stock-backups#daikin-esp32-and-screen). Command reference: [SEGGER J-Link Commander](https://kb.segger.com/J-Link_Commander).
