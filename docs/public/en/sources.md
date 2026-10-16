# Asset sources

Guide images and tools are hosted under `guides/assets/`. [SHA256SUMS](assets/SHA256SUMS) lists their checksums.

| File | Source |
| --- | --- |
| `images/emw3080-pins.png` | [MXCHIP EMW3080 pin diagram](https://docs.mxchip.com/img/zh/1631842737419-34af0113-6807-499a-9ff9-129040293369.png); original content retained, transparent background changed to white |
| `images/emw3080-swd-pins.png` | SWD pin diagram from the [official EMW3080 flashing guide](https://docs.mxchip.com/zh/foiwqd8gbgh23xu2/bwyo0fs5t2rquekz/antua5wkt8m3qx7l/lqdz2h5zpp117imt.html) |
| `images/tc1-a1-module.png`, `images/tc1-a1-test-points.png` | User-provided annotated TC1 A1 module / factory test-point images |
| `images/a1-factory-test-points.png` | User-provided annotated A1 purifier factory test-point photo |
| `images/jlink-swd.png` | [Official SEGGER SWD diagram](https://www.segger.com/fileadmin/images/products/J-Link/Technology/Interface_Description/181129_SWD.svg), converted to PNG on white |
| `images/flashplus-config.png` | Configuration crop from page 2 of the [EMW3031 / FlashPlus manual](https://github.com/IoTDevice/phicomm_tc1_a1/blob/master/工具和文档/EMW3031说明文档_flashplus.pdf). Used only for chip / Flash identification; use this project's erase and verification instructions |
| `tools/FlashPlus_v1.0.10.msi` | Marvell FlashPlus v1.0.10 from the [IoTDevice mirror](https://github.com/IoTDevice/phicomm_tc1_a1/blob/master/工具和文档/FlashPlus_v1.0.10.msi), unmodified installer |

Images and third-party tools remain copyrighted by their authors and vendors; hosting copies does not change their licenses. FlashPlus is not part of this project's firmware source. The EMW3031 datasheet restricts reproduction of its illustrations, so the guide links to it instead.

See [Build dependencies](build-deps) for toolchains and SDKs.
