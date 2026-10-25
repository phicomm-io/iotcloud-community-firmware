# Firmware updates

This page covers web OTA after community firmware is installed. Factory devices need their model-specific first installation. M1 can use [factory LAN migration](m1-no-disassembly), **starting with 0.9.21 or newer**; do not upload the community signature wrapper to the factory interface.

**M1 has two MCUs.** This page updates the EMW3080 Wi-Fi module. It does not update the ATSAMD20G18A sensor controller. Some Wi-Fi v0.9.27 features require a separate controller update through the API; see [controller features](m1-mcu-firmware) and [controller update steps](m1-mcu-update).

## Check for updates on the device

Official A1, M1 and TC1 builds provide GitHub Pages HTTPS and domestic/international HTTP update sources. Choose a source in the firmware settings and check:

- The UI distinguishes **a new version**, **already up to date** and **network failure**.
- Switch sources and retry if a network request fails.
- Check the model, version and release notes, then explicitly download and install. **Checking never installs an update automatically.**

You can read release notes for the selected version. Use **Cancel download** during transfer; cancellation, network timeout or failure restores normal services. A retry starts from the beginning. Cancellation is unavailable after the final commit starts.

| Source | Address |
| --- | --- |
| GitHub Pages | `https://phicomm-io.github.io` |
| China mirror | `http://phicomm.fjrcn.cn` |
| International source | `http://phicomm.fjrcn.com` |

## Compare changes across releases

Open the website's [Release Log upgrade comparison]({{SITE_URL}}guides/en/release-log.html?view=compare) and choose the model, firmware variant, current version and target release. Notes follow the actual release chain. Pending reviews, missing translations or mismatched archives stop the comparison instead of substituting the latest release notes.

Optionally enter a LAN address and read the device information once. The web management password is used only for that request; manual selection remains available if reading fails. A comparison never checks for or installs firmware. Older device pages retain their original single-release notes.

## Choose the correct file

| File | Purpose |
| --- | --- |
| `.ota.bin.signed` | Signed OTA package for web upload and MQTT / URL installation; P-256 / SHA-256 signature verification is mandatory |
| `.ota.bin.raw` | Native image for an explicitly supported first migration or unsigned recovery; factory M1 migration uses the full image from 0.9.21 or newer |
| `.all.bin` | Full image for wired flashing; **never upload it as OTA** |

Use the GitHub archive on the [project downloads page]({{SITE_URL}}#releases), choose your exact product and verify `SHA256SUMS` from that release. The device verifies the header signature before erasing, and activates the new image only after Flash readback SHA-256 and compatibility checks pass.

## Install a local file

Online demos use simulated data. On a real device:

1. Open its firmware update page.
2. Select and upload the verified `.ota.bin.signed` file.
3. Keep power connected throughout writing and the automatic reboot.
4. Reopen the WebUI and check the running version.

Unsigned recovery from older community firmware may use `.ota.bin.raw` under its specific procedure. Firmware supporting that option accepts it after selecting **Allow unsigned firmware**. Factory M1 uses the separate [factory upload procedure](m1-no-disassembly); later normal updates still use signed packages.

## Recovery

Updates pause linkage and other business responses while keeping update status and cancellation available. Reduce concurrent access during an update; sustained multi-client pressure updating has not passed full acceptance.

If the WebUI is reachable, check the file, model, version and network, then retry. If the device cannot boot, use the corresponding wired recovery guide: [TC1](tc1-flash), [M1 / A1](a1-m1-flash), [DAIKIN](daikin-guide). Normal OTA updates do not require opening the device.

## Full and slim firmware

DAIKIN BRY88AX151K has one universal full image that detects PM2005 / PM2105 and the optional formaldehyde module. Use the local file procedure above; online checks and URL downloads are unavailable.

Default files include the complete device UI. Files containing `-no-frontend-resources` keep an offline setup page for Wi-Fi, initial MQTT settings and the web password, available through the device hotspot or LAN address. Use [local device management]({{SITE_URL}}local/) or IoTCloud for full controls and settings. Both share the same appearance. MQTT is not required to complete Wi-Fi setup.

Online checks stay on the current firmware variant. To switch variants manually, select the signed package for the same physical product.
