# Asset sources

Guide images and tools are hosted under `guides/assets/`. [SHA256SUMS](assets/SHA256SUMS) lists their checksums.

| File | Source |
| --- | --- |
| `images/tc1-ota-*.png`, `images/tc1-stock-*.png`, `images/tc1-community-ap.png` | User-provided no-disassembly flashing screenshots, cropped to key router, network, TFTP, stock identity and first-boot areas |
| `tools/tc1-no-disassembly/mico_ota.bin` | The same validated image snapshot as the parent guide (raw image SHA-256: `005b5c4d8a1bb4e70b1b6aa84f1c403e648d3c2d4d683a067a22193543050735`) plus its 16-byte binary MD5; fixed tutorial file, not automatically replaced by the latest release |
| `tools/tc1-no-disassembly/tftp-trigger.bin` | 397-byte trigger generated from the parent guide, only for TC1 A1 stock 1.0.0.26, not a general firmware update package |
| `images/emw3080-pins.png` | [MXCHIP EMW3080 pin diagram](https://docs.mxchip.com/img/zh/1631842737419-34af0113-6807-499a-9ff9-129040293369.png); original content retained, transparent background changed to white |
| `images/emw3080-swd-pins.png` | SWD pin diagram from the [official EMW3080 flashing guide](https://docs.mxchip.com/zh/foiwqd8gbgh23xu2/bwyo0fs5t2rquekz/antua5wkt8m3qx7l/lqdz2h5zpp117imt.html) |
| `images/tc1-a1-module.png`, `images/tc1-a1-test-points.png` | User-provided annotated TC1 A1 module / factory test-point images |
| `images/a1-jlink-points.png` | User-provided corrected A1 J-Link wiring photo (DIO above CLK) |
| `images/m1-scd40-wiring.png` | User-provided M1 / SCD40 wiring photo; handwritten 7 / 8 are GPIO signal names |
| `tools/emw3080-openocd.zip`, `tools/emw3080-openocd/` | Project Python 3 validation, splitting and flashing helper with modern MX1290 configuration; no third-party binaries or firmware. Requires the matching programmer and script from the [original zA1 package](https://github.com/a2633063/zA1/blob/f4f67ee88d9287fd1badff55ccd7704bb0f748c4/Jlink烧录文档/3080_JLINK烧录.zip) |
| `images/jlink-swd.png` | [Official SEGGER SWD diagram](https://www.segger.com/fileadmin/images/products/J-Link/Technology/Interface_Description/181129_SWD.svg), converted to PNG on white |
| `images/flashplus-config.png` | Configuration crop from page 2 of the [EMW3031 / FlashPlus manual](https://github.com/IoTDevice/phicomm_tc1_a1/blob/master/工具和文档/EMW3031说明文档_flashplus.pdf). Used only for chip / Flash identification; use this project's erase and verification instructions |
| `tools/FlashPlus_v1.0.10.msi` | Marvell FlashPlus v1.0.10 from the [IoTDevice mirror](https://github.com/IoTDevice/phicomm_tc1_a1/blob/master/工具和文档/FlashPlus_v1.0.10.msi), unmodified installer |
| `tools/a1-easylink.zip` | Project-owned A1 EasyLink tools: `tools/a1_easylink.py`, `tools/a1_easylink_web.py` and three web assets; no firmware included; archive checksum in SHA256SUMS |

Images and third-party tools remain copyrighted by their authors and vendors; hosting copies does not change their licenses. FlashPlus is not part of this project's firmware source. The EMW3031 datasheet restricts reproduction of its illustrations, so the guide links to it instead.

## Operating notes

The A1 filter and power interlock note comes from [zA1 Issue #3](https://github.com/a2633063/zA1/issues/3). Users reported that the purifier would not power on without its filter; the original project maintainer confirmed adding the reminder to the documentation.

See [Build dependencies](build-deps) for toolchains and SDKs.
