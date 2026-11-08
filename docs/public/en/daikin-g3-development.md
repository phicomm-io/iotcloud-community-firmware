# G3 architecture, protocols and development resources

G3 has two independent projects. ESP32 owns networking, IoTCloud, the WebUI, weather and screen OTA. The SWM34SVET6-50 screen MCU owns sensors, charging control, buttons, RTC, LCD and LVGL. ESP32 GPIO16 TX / GPIO17 RX communicate at 115200 8N1, retaining factory business messages and adding negotiated extensions.

## Current development baseline

| Project | References |
| --- | --- |
| ESP32 | [RC6 source](https://github.com/phicomm-io/iotcloud-community-firmware/tree/v1.0.0-rc.6/BRY88BBA15G), [build and wiring](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/README.md) |
| Screen MCU | [Independent RC6 repository](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/tree/v1.0.0-rc.6), [build instructions](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/README.md) |
| Extended services | [Parameters, capabilities, binary layouts and OTA](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/SERVICES.md) |
| Web and platform | [Shared frontend]({{FRONTEND_URL}}), [IoTCloud]({{PLATFORM_URL}}), [device protocols](device-api) |
| Operation | [Layouts](daikin-g3-layout), [weather and pushes](daikin-g3-services), [diagnostics](daikin-g3-diagnostics), [stock backups](stock-backups) |

The WebUI uses MessagePack. Factory UART messages use JSON; extension frames carry session, sequence and CRC fields. Stock MCUs without extensions retain the factory communication path.

## Board responsibilities and recovery boundaries

The screen side reads temperature/humidity, CO₂, AM1008L particles/TVOC and optional HCHO. ETA6964 participates in battery charging management. PD11 is the front touch key, PD13 the recessed rear network button, and PD12 controls AM1008L SET, confirmed by module on/off comparisons. P50 is the physically identified nRESET test point; verify the particular board before wiring or probing.

The application starts at `0x14000`; serial OTA preserves the original first 80 KiB boot region. Internal SRAM prioritizes communication, fault recovery and stacks. SDRAM holds framebuffers, LVGL and verified resources. External NOR stores fonts/wallpapers, with bounded failure and fallback. See the references below for watchdog ownership and sensor-compensation order.

## Installation and recovery

First installation sends the complete 421888-byte application from ESP32 through the factory UART updater. Existing community MCUs use NOR staging, readback verification and activation. The WebUI and `flash_screen.py --mode install` select the path automatically. Success requires a new MCU session, a matching internal application MD5 and normal operating mode.

The application starts at `0x14000`; serial updates preserve the first 80 KiB factory boot region. Fonts and wallpapers reside in external NOR independently of application OTA. The optional `backup_screen_boot.py` tool reads the community MCU boot region twice and compares the results. The installer can reuse a backup with `--boot-backup DIRECTORY --reuse-boot-backup`. The stock MCU has no such readout interface. A recovery image starts only minimal services in internal SRAM, with communication and updates available while the display stays black.

The factory loader leaves PLL output enabled at handoff. Before changing dividers, the application waits for crystal settling, disables the PLL and its output, reconfigures it and requires two consecutive lock reads. Lock acquisition is bounded; failure leaves low-clock UART recovery available. This fixes the reproduced SDRAM initialization failure following low-clock startup. See [clock initialization](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/55e52f3e922f174b6c515279c8b3d742e42453ae/platform/swm34s/CMakeLists.txt).

Release sites may compress HTTP transfers. Browser `fetch` returns decoded data, while `Content-Length` may describe the compressed transfer. Online installation checks the actual application length, followed by ESP32 SHA-256 verification. See the [frontend fix](https://github.com/phicomm-io/iotcloud-community-frontend/commit/6034bf88ac3be09f3b849ca11f9e7d55fe0335d3) and its compressed, missing-length and truncated-response checks.

### SWD tooling and write range

User-facing Windows / Linux / macOS wiring and commands are in the [flashing guide](daikin-g3-flash#screen-mcu-installation-with-j-link). The screen uses STAR-MC1. [Synwit's J-Link plugin](https://github.com/Synwit-Co-Ltd/JFlash/blob/7dc3a294fbfc23b7e533006ad301354f597777a6/JLinkDevices.xml) defines `SWM341xE` with `JLINK_CORE_CORTEX_M33` and the internal Flash algorithm `SWM341xE.FLM`. A generic core name without the Flash algorithm is insufficient.

The normal RC6 application is `0x67000` bytes, written to `[0x14000, 0x7b000)`. Preserve the first `0x14000` boot bytes, the trailing gap and configuration at `[0x7e000, 0x80000)`. Read two matching `0x80000`-byte SWD backups before writing; compare application readback with the published digest afterward. External NOR is outside this internal Flash operation.

## Power and OTA scheduling

The stock BRY88BBA15G ESP32 `V01.02.00` uses ESP-IDF 5.0.1. Its power configuration sets an 80 MHz maximum, 10 MHz minimum and automatic light sleep. `sensor3` and `uart_resp` use APB frequency locks for communication. Configuration data at `0x3f4060d8` is passed to `esp_pm_configure` at `0x400d8e04`; the stock boot log confirms 80 MHz.

Stock OTA downloads over HTTPS and coordinates with the MCU over UART. It reads `0x800` bytes per iteration and calls `vTaskDelay(2)`. `esp_ota_begin(next_partition, -1, ...)` erases the inactive partition before download; MD5 verification, OTA completion and boot-partition switching follow. G3 uses this path; the G1/G2 TCP 8555 tool is incompatible.

Community G3 runs at a fixed 80 MHz. Its current Arduino SDK has dynamic power management and tickless idle disabled. Upload writes are at most 512 B, online downloads use a 1024 B buffer, and execution yields every 16 KiB. Flash sectors are erased as data arrives. Stock-style dynamic frequency scaling and light sleep also require SDK configuration changes.

Screen startup completes font loading and verification before enabling sensors. The three sensor UARTs retain independent interrupt buffers. Updates dim the display and pull PD12 SET low. ESP32 renews module sleep through a bounded lease and matching acknowledgements while keeping the UART owner receiving. Flash writes can drop periodic status, so an acknowledged lease does not depend on every announcement. ACK age uses the current time after reception, avoiding unsigned underflow from a stale loop timestamp. See [module control](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/screen_module.inc).

After an ESP32-only reboot, ESP32 requests fresh screen capability negotiation. If the startup request is lost, receiving an old-session extension frame with a valid CRC triggers another reset request, restoring capability, layout and display-policy synchronization without restarting the MCU. See the [UART bridge](https://github.com/phicomm-io/iotcloud-community-firmware/blob/v1.0.0-rc.6/BRY88BBA15G/bridge.c).

## Memory and resources

SCN3 layouts occupy 9520 bytes with 288-byte component records. The UART cache prefers 32-bit IRAM with aligned word access, falling back to DRAM when allocation fails. `screen.status` fields `layout_cache_bytes` and `layout_cache_storage` report its size and `iram` / `dram` / `unallocated` location. Status-encoding scratch space and layout upload buffers are allocated on demand and released on completion or timeout.

TLS uses byte-addressable internal DRAM. Open-Meteo admission requires 72 KiB byte-addressable heap and a contiguous block of at least 24 KiB; other providers require 84 KiB heap. `screen.weather.get` reports these metrics, worker heap levels, attempts and backoff. The factory path uses `delivery:factory`; `factory` holds the last temperature, humidity, condition and outdoor PM2.5 fields written to UART, with age in `factory_sent_age_ms`.

NOR defaults to hardware quad SFC for fonts, wallpapers and update staging, including writes and readback. Single SFC or GPIO requires explicit ESP32 selection during diagnostic startup; there is no automatic bus switching. Fonts are verified during incremental reading and then cached in SDRAM. Errors and timeouts use built-in fonts and a solid background. See the [board implementation](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/55e52f3e922f174b6c515279c8b3d742e42453ae/platform/swm34s/board.c) and [startup implementation](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/55e52f3e922f174b6c515279c8b3d742e42453ae/platform/swm34s/application.c).

## Display measurements

A capture holds up to 96 records, read in batches of eight. All batches in a report use the same generation and include drop counts. Scan periods come from LCD frame-end interrupts and include interrupt latency. Flush time includes pixel copying; monitor records describe each LVGL refresh; adjacent flush intervals include drawing and scheduling.

The current driver uses one scan framebuffer and a 40-line draw buffer, usually requiring 22 copies for a full page. The stock driver uses two complete buffers and loads the display address at frame boundaries. Diagnostics do not change scan timing or buffering. See the display performance documents below for layout and driver measurements.

## Screen documentation index

These links are pinned to screen RC6. Some files record earlier stages or experiments; current protocols and headers take precedence over historical layout/resource formats.

| Topic | Documents |
| --- | --- |
| Implementation history | [IMPLEMENTATION](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/IMPLEMENTATION.md), [2026-11-05 implementation](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/IMPLEMENTATION-20261105.md) |
| Serial and negotiation | [screen-link](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/screen-link.md) |
| Boot, watchdog and recovery | [BOOTLOADER](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/BOOTLOADER-20261105.md), [RECOVERY-WDT](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/RECOVERY-WDT.md), [MCU-RECOVERY](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/MCU-RECOVERY-20261106.md) |
| Validation and first installation | [VALIDATION](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/VALIDATION.md), [FLASH-TEST](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/FLASH-TEST-20261105.md) |
| Startup and sensors | [STARTUP-LOADING](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/STARTUP-LOADING.md), [SENSORS-CALIBRATION](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/SENSORS-CALIBRATION.md), [MODULE-CONTROL](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/MODULE-CONTROL.md), [RTC](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/RTC.md) |
| UI and appearance | [UI-REDESIGN-PLAN](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/UI-REDESIGN-PLAN.md), [UI-AND-RESOURCES](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/UI-AND-RESOURCES.md), [APPEARANCE](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/APPEARANCE.md), [BRIGHTNESS-PLAN](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/BRIGHTNESS-PLAN.md) |
| Fonts | [EXTERNAL-FONT](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/EXTERNAL-FONT.md) |
| Display performance | [DISPLAY-DIAGNOSTICS](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/DISPLAY-DIAGNOSTICS.md), [RENDER-PERFORMANCE](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/RENDER-PERFORMANCE.md), [PAGE-SWITCH-PERFORMANCE](https://github.com/phicomm-io/iotcloud-bry88bba15g-screenmcu/blob/v1.0.0-rc.6/docs/PAGE-SWITCH-PERFORMANCE.md) |

## Build and acceptance

Build ESP32 and ARM applications separately. Check application limits, protocol snapshots, recovery services, resource validation and signed manifests. Record source revisions, image digests, installation results and device health after reboot. Display changes need scan/flush/drawing measurements and OTA responsiveness. Power/sensor changes need cold-start, module-start, charging and fault-maintenance tests. Get published packages and notes through [Firmware & resources]({{SITE_URL}}en/#releases).
