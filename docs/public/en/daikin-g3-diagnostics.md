# G3 startup, resource and display diagnostics

For the [current RC6 release]({{SITE_URL}}en/#releases), first check ESP32 and screen MCU versions, resource status and uptime. See the [main guide](daikin-g3-guide) for installation/recovery and [stock backups](stock-backups) for original components.

## Startup and recovery window

Screen communication and recovery start first. ESP32 normally continues business startup after five seconds; disabling auto-continue retains a 30-second minimal-service window. Hold minimal service overrides auto-continue, while Continue now exits early. Fault maintenance never exits automatically. A loading animation covers font and sensor startup; failed items have bounded waits.

After complete power loss, the retained stock boot gate may still require a short front-key touch. This release does not modify the boot region for automatic power-on. Ordinary short touches change pages; the rear recessed button controls network functions and is not a general reset action.

MCU updates temporarily reduce brightness to 10% and hold the AM1008L PD12 SET signal low, restoring it when business startup is ready. A manual diagnostic low lease lasts at most 60 seconds, restores automatically and never persists as a permanent off setting. Recovery and updates use an independent module-off interlock.

For black screens or restarts, record ESP32 and MCU uptimes/reset reasons separately, power status and module connections. A dark screen does not establish that ESP32 disconnected, and an online WebUI does not establish that screen business startup completed. Keep wiring and supply fixed during comparisons and avoid repeated live hot-plugging.

## Resources and missing glyphs

The complete 32 px Chinese font resides in external SPI NOR and enters SDRAM after verification. Startup uses a compact built-in font until background loading completes. If custom Chinese city text shows placeholders, inspect font status/progress; entering a city label does not download a font.

Hardware reads, chunked verification and fallback paths have bounded timeouts. Reads can fall back to GPIO; unavailable resources fall back to built-in fonts, missing-glyph placeholders and solid backgrounds. Communication, sensors and OTA continue. Resource errors must not cause endless reboot loops. Fonts and wallpapers update separately and need no repeat upload for an ordinary application OTA.

## ESP32 memory and weather

Total free memory includes word-only IRAM, while TLS and common network buffers need byte-addressable internal DRAM. Check byte heap, its largest contiguous block and minimum watermark instead of relying on the total.

RC6 releases the idle 6 KiB status-encoding scratch pool and completed delayed-UART String storage. Its 9520-byte SCN3 wire cache first uses available 32-bit IRAM with aligned word accesses, falling back to DRAM. Layout encoding/upload buffers are allocated on demand and freed after completion or timeout.

`screen.status` reports `layout_cache_bytes` and `layout_cache_storage` as `iram`, `dram` or `unallocated`. `screen.weather.get` includes byte heap, largest block, worker start/end values, attempts, consecutive failures and retry delay. Open-Meteo admission currently requires 72 KiB byte heap and a 24 KiB contiguous block; other providers require 84 KiB. A 100 KiB total can still fail these conditions. Allocation failure backs off while preserving cached data.

These optimizations passed builds and host checks. Actual IRAM placement and weather recovery still require updated-device status. Pausing weather and avoiding concurrent large transfers provide a low-load comparison, not a substitute for memory measurements.

## Measure page changes

Under Screen → Diagnostics, select the current page, a chosen page or the next transition, then trigger paging and read/export the trace. Controls require MCU support. A capture retains 96 records, fetched in batches of eight. Keep one generation throughout and check dropped records.

| Metric | Meaning |
| --- | --- |
| Scan period | LCD frame-end IRQ cadence, including interrupt latency; not a physical VSYNC probe |
| Flush rectangles, count and duration | Includes actual pixel copies for synchronous flush, with coordinates and last-chunk flags |
| Monitor rounds, pixels and time | LVGL refresh rounds; one page transition may contain several |
| Inter-flush gaps | Include drawing and scheduling; they do not establish VSYNC waits |

The current driver uses one scanning framebuffer and a 40-row drawing buffer, normally requiring 22 full-page copies without a per-chunk VSYNC wait. A saved-layout comparison on 2026-11-07 measured about 40 Hz scanning, 613–693 ms LVGL refreshes and approximately 141 ms total copying. These historical measurements do not guarantee performance for every layout.

The reviewed stock driver uses two complete framebuffers with a frame-boundary address reload. Further work must compare opaque fills, predecoded pictures and complete pages, verify independent A/B addresses, unchanged-region synchronization, bounded swaps and OTA responsiveness, then evaluate DMA2D. Diagnostics alone do not change scan clocks or enable double buffering. See the display/performance reports in [development resources](daikin-g3-development).
