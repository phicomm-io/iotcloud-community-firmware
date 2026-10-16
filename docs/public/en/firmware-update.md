# Firmware updates

Web OTA is available **only after this community firmware is installed**. Phicomm factory firmware and other third-party firmware require a first installation using the appropriate flashing guide.

## Check for updates on the device

Official builds provide domestic and international update sources. Choose a source in the firmware settings and check:

- The UI distinguishes **a new version**, **already up to date** and **network failure**.
- Switch sources and retry if a network request fails.
- Check the model, version and release notes, then explicitly download and install. **Checking never installs an update automatically.**

Custom builds receive their sources through `PHICOMM_OTA_CN_BASE_URL` and `PHICOMM_OTA_GLOBAL_BASE_URL`. Without those settings, built-in checking is hidden; manual URL and file uploads remain available.

| Source | Address |
| --- | --- |
| China mirror | `http://phicomm.fjrcn.cn` |
| International source | `http://phicomm.fjrcn.com` |

## Choose the correct file

| File | Purpose |
| --- | --- |
| `.ota.bin.signed` | Signed OTA package for web upload and MQTT / URL installation; P-256 / SHA-256 signature verification is mandatory |
| `.ota.bin.raw` | Raw image for an explicitly selected unsigned recovery procedure |
| `.all.bin` | Full image for wired flashing; **never upload it as OTA** |

Use the GitHub archive on the [project downloads page]({{SITE_URL}}#releases), choose your exact product and verify `SHA256SUMS` from that release. The device verifies the header signature before erasing, and activates the new image only after Flash readback SHA-256 and compatibility checks pass.

## Install a local file

Online demos use simulated data. On a real device:

1. Open its firmware update page.
2. Select and upload the verified `.ota.bin.signed` file.
3. Keep power connected throughout writing and the automatic reboot.
4. Reopen the WebUI and check the running version.

A first transition from older firmware may require `.ota.bin.raw`. New firmware accepts it after selecting **Allow unsigned firmware**, without a second confirmation dialog.

## Recovery

If the WebUI is reachable, check the file, model, version and network, then retry. If the device cannot boot, use the corresponding wired recovery guide: [TC1](tc1-flash), [M1 / A1](a1-m1-flash). Normal OTA updates do not require opening the device.
