# Firmware release gates

CI builds the real full and hub DAIKIN images and checks the application binary
and ELF before collecting release assets. `check_esp_budget.py --layout stock`
requires the application plus 32 KiB of headroom to fit the original **0x180000
(1,572,864 byte)** OTA slot. A stock application migration keeps the bootloader
and partition table, so this limit remains for later community OTA updates.

The repository's `DAIKIN/partitions.csv` describes the **wired installation**
layout, with **0x1e0000 (1,966,080 byte)** OTA slots. Passing only
`--layout wired` does not establish compatibility with a device that retained
the stock table. The tool never writes a partition table or changes a device.
Its JSON report includes both capacities and both compatibility results, the
selected gate, the measured binary/ELF hashes and the static RAM budget. Failed
size gates also produce a report; CI preserves that report and stops signing.

The ten current build combinations are TC1 full/slim, M1 full/slim, A1
full/slim/fresh-air/fresh-air-slim, and DAIKIN full/hub. `products.py --targets`
is the shared enumeration for building the signing/catalog matrix. DAIKIN v1
catalog entries authenticate each signed payload's exact full/hub image label
and require the downloadable raw application to be the same payload.

Only stable v1+ tags at a commit on `main` may publish firmware. Branch builds
and a manual signing dry run do not promote an OTA manifest. New manifests use
`/ota/v1/<model>/<variant>/latest.json`; legacy paths stop at the v1.0.0 bridge.
The v0.x archive retains its original asset and metadata shape.
