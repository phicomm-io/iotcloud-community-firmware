# Build dependencies

This guide covers toolchains, SDK archives and local builds for all three products. Users of prebuilt firmware can go directly to [downloads]({{SITE_URL}}#releases).

Download dependencies from the [0.0.0 archive](https://github.com/phicomm-io/phicomm-community-firmware/releases/tag/0.0.0) and verify the included checksums:

```sh
sha256sum -c SHA256SUMS
```

## Files

| File | Purpose and source |
| --- | --- |
| [gcc-arm-none-eabi-5_4-2016q2-20160622-linux.tar.bz2](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/gcc-arm-none-eabi-5_4-2016q2-20160622-linux.tar.bz2) | Linux x86_64 ARM GCC 5.4.1 for all three products; [upstream download](https://launchpad.net/gcc-arm-embedded/5.0/5-2016-q2-update) |
| [gcc-arm-none-eabi-5_4-2016q2-20160622-src.tar.bz2](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/gcc-arm-none-eabi-5_4-2016q2-20160622-src.tar.bz2) | Corresponding toolchain source and build scripts, retaining original licenses |
| [Python-2.7.18.tgz](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/Python-2.7.18.tgz) | Python source needed by the legacy SDK build; [official archive](https://www.python.org/ftp/python/2.7.18/Python-2.7.18.tgz) |
| [mico-os-3.6.4.bundle](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/mico-os-3.6.4.bundle) | Git copy of the M1 / A1 SDK at [MXCHIP/mico-os](https://github.com/MXCHIP/mico-os/tree/6c465211d3ff8797cd835e400ec54a06530dd476), commit `6c465211d3ff8797cd835e400ec54a06530dd476`, without project patches |
| [tc1-mico-sdk.tar.gz](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/tc1-mico-sdk.tar.gz) | TC1 MiCO 3.5.4 SDK and third-party licenses, verified and extracted into `build/tc1-sdk/` |

Toolchain archives retain their licenses; corresponding GCC component source is provided. Python uses its bundled PSF license. MiCO includes differently licensed source and binary components; their original declarations apply, rather than a blanket project license. Copyright and license notices remain in the archives.

## Prepare local copies

Create a writable shared tools directory, such as `/opt/phicomm-tools`. Extract GCC there and build Python into `python2/`:

```sh
tar -xjf gcc-arm-none-eabi-5_4-2016q2-20160622-linux.tar.bz2 -C /opt/phicomm-tools
tar -xzf Python-2.7.18.tgz
cd Python-2.7.18
./configure --prefix=/opt/phicomm-tools/python2
make -j4
make install
```

M1 / A1 require **Git clones**, not an unpacked source tree, because the build checks commit identity. Restore separate copies from the project root to keep product patches separate:

```sh
mkdir -p build/m1-sdk build/a1-sdk
git clone /path/to/mico-os-3.6.4.bundle build/m1-sdk/mico-os
git clone /path/to/mico-os-3.6.4.bundle build/a1-sdk/mico-os
git -C build/m1-sdk/mico-os rev-parse HEAD
```

The result must be the pinned commit above.

## Build

From the project root:

```sh
TC1_TOOLS_DIR=/opt/phicomm-tools ./TC1/build.sh
M1_TOOLS_DIR=/opt/phicomm-tools ./M1/build.sh
A1_TOOLS_DIR=/opt/phicomm-tools ./A1/build.sh
```

The scripts apply product-specific SDK patches. Correct existing GCC / Python copies are reused without downloading. Linux also needs make, Git, Perl, Python 3, bzip2, curl, a C build environment and related system libraries; see the [project build instructions](https://github.com/phicomm-io/phicomm-community-firmware#readme).

Use `TC1_SDK_ARCHIVE` for an already downloaded TC1 SDK archive. Run `./build.sh` at the project root to build all three products in sequence.
