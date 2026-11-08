# G3 startup, resource and display diagnostics

Check ESP32 and screen MCU versions, uptime and resource status in device information. Use the [current matching release]({{SITE_URL}}en/#releases); installation steps are in the [main guide](daikin-g3-guide).

## Startup and a black screen

The screen opens a maintenance window before starting normal operation, automatically continuing after 5 seconds. Disabling automatic continuation gives a 30-second window. **Keep minimal services** pauses normal startup; **Continue startup now** ends the wait. Fault maintenance requires a repaired image.

After complete power loss, touch the front key to start. Short presses change pages or wake the screen. With a black display in maintenance, hold about 3 seconds to shut down, release, then hold about 2 seconds to start again. Power-key shutdown is paused during firmware transfer.

For a black screen or reboot, open the device WebUI and check MCU mode, fault stage and both uptimes to distinguish unfinished startup from a device reset. Keep stable USB power connected and record versions and reset reason before updating. The rear network key is for provisioning; a long press triggers reset.

Updates temporarily dim the screen and pause the PM2.5/TVOC module. Brightness and the module resume afterward; check that both readings refresh.

## Fonts, missing glyphs and wallpaper

Startup uses the built-in font while the full font loads in the background. If custom Chinese characters appear as placeholders, check font installation, status and progress. Setting a city name does not download the font.

Reinstall the affected font or wallpaper if resource verification fails. Unavailable resources use the built-in font or a solid background. Ordinary MCU updates preserve these resources. Failed wallpaper uploads preserve the previous image.

## ESP32 memory and weather

The weather page shows the latest error and retry countdown. Check enablement, coordinates, Key/Host and device time first. If proxy DNS returns a virtual address, verify that the gateway routes it through the proxy.

For a memory error, wait for layout/resource uploads or OTA to finish before refreshing. Weather requests automatically back off and preserve the previous cache. For persistent memory errors, record byte-addressable free heap, largest contiguous block, minimum heap and attempt count; see [Development](daikin-g3-development#memory-and-resources).

## Export display diagnostics

MCUs with diagnostic support show a capture entry under **Screen → Diagnostics**. Select the current page, a specific page or the next page change, trigger the operation, then read and export the complete report.

Attach the layout, screen version and report when reporting page-change problems. Reports contain scan periods, refresh regions, copy and drawing times; see the definitions in [Development](daikin-g3-development#display-measurements).
