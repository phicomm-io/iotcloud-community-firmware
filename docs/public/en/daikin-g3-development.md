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

RC6 uses the current SCN3 layout ABI: 9520 bytes with 288-byte component records. Older community layout ABIs are not new negotiation targets. Stock MCUs without extensions retain the factory communication path. UART extensions still carry session, sequence and CRC framing; WebUI MessagePack, factory UART JSON and extended payloads are different layers.

## Board responsibilities and recovery boundaries

The screen side reads temperature/humidity, CO₂, AM1008L particles/TVOC and optional HCHO. ETA6964 participates in battery charging management. PD11 is the front touch key, PD13 the recessed rear network button, and PD12 controls AM1008L SET, confirmed by module on/off comparisons. P50 is the physically identified nRESET test point; verify the particular board before wiring or probing.

The application starts at `0x14000`; serial OTA preserves the original first 80 KiB boot region. Internal SRAM prioritizes communication, fault recovery and stacks. SDRAM holds framebuffers, LVGL and verified resources. External NOR stores fonts/wallpapers, with bounded failure and fallback. Verify watchdog ownership and the original sensor-compensation operation order from the references. A successful command reply alone does not establish that calibration ran.

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

Build ESP32 and ARM applications separately. Check application limits, protocol snapshots, recovery services, resource validation and signed manifests. Host tests, CI builds, downloaded-byte verification and physical operation are separate evidence. Display changes need scan/flush/drawing measurements and OTA responsiveness. Power/sensor changes need cold-start, module-start, charging and fault-maintenance tests. Get published packages and notes through [Firmware & resources]({{SITE_URL}}en/#releases).
