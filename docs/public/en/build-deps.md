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
| [mico-os-3.6.4.bundle](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/mico-os-3.6.4.bundle) | Shared Git SDK copy for all three products at [MXCHIP/mico-os](https://github.com/MXCHIP/mico-os/tree/6c465211d3ff8797cd835e400ec54a06530dd476), commit `6c465211d3ff8797cd835e400ec54a06530dd476`, without project patches |
| [tc1-mico-sdk.tar.gz](https://github.com/phicomm-io/phicomm-community-firmware/releases/download/0.0.0/tc1-mico-sdk.tar.gz) | Historical TC1 MiCO 3.5.4 archive for older revisions; current builds use MiCO 3.6.4 above |

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

All products read the SDK source and pinned commit from `common/mico-sdk.json`. The build downloads upstream once into `build/mico-upstream.git`, then creates a separate Git checkout for each product's board patches. A1 and M1 initialize GPIOs differently, so they must keep separate patched working directories.

To use an offline Git bundle, set its path before building:

```sh
export MICO_SDK_REPOSITORY=/path/to/mico-os-3.6.4.bundle
./build.sh
```

`MICO_SDK_CACHE` selects a different absolute path for the shared source cache. Every checkout must match commit `6c465211d3ff8797cd835e400ec54a06530dd476`; an incorrect commit or incomplete checkout stops the build. Defaults are `build/tc1-sdk-3.6.4/mico-os`, `build/m1-sdk/mico-os` and `build/a1-sdk/mico-os`. Each product's `*_SDK_DIR` may point to a separate Git checkout at the same commit.

## Build

From the project root:

```sh
TC1_TOOLS_DIR=/opt/phicomm-tools ./TC1/build.sh
M1_TOOLS_DIR=/opt/phicomm-tools ./M1/build.sh
A1_TOOLS_DIR=/opt/phicomm-tools ./A1/build.sh
```

The scripts apply product-specific SDK patches. Correct existing GCC / Python copies are reused without downloading. Linux also needs make, Git, Perl, Python 3, bzip2, curl, a C build environment and related system libraries; see the [project build instructions](https://github.com/phicomm-io/phicomm-community-firmware#readme).

Current builds no longer download the TC1 3.5.4 archive. Run `./build.sh` at the project root to build all three products in sequence.
