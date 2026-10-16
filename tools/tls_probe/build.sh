#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
work="$PWD/build/tls-probe"
sha_dir="${OTA_SHA_DIR:-$PWD/build/a1-sdk/mico-os/MiCO/security/SHAUtils}"
[[ -f "$sha_dir/sha224-256.c" ]] || { echo 'Set OTA_SHA_DIR to the MiCO SDK SHAUtils directory' >&2; exit 1; }
mkdir -p "$work"
if [[ ! -d "$work/bearssl-0.6" ]]; then
  curl -fLsS https://bearssl.org/bearssl-0.6.tar.gz -o "$work/bearssl.tar.gz"
  echo "6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14  $work/bearssl.tar.gz" | sha256sum -c -
  tar -xzf "$work/bearssl.tar.gz" -C "$work"
fi
src="$work/bearssl-0.6"
make -C "$src" -j8 lib >"$work/make.log" 2>&1
sources=(tools/tls_probe/download.c tools/tls_probe/profile.c common/ota/signed_ota.c common/ota/crypto.c "$sha_dir/sha224-256.c")
includes=(-I"$src/inc" -Icommon/ota -I"$sha_dir")
cc -Os -Wall -Wextra -Wno-misleading-indentation "${includes[@]}" tools/tls_probe/client.c "${sources[@]}" "$src/build/libbearssl.a" -o "$work/client"
if [[ -n "${ARM_PREFIX:-}" ]]; then
  flags=(-Os -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -fno-common -ffunction-sections -fdata-sections -fstack-usage)
  make -C "$src" -j8 lib BUILD=arm CC="${ARM_PREFIX}gcc" AR="${ARM_PREFIX}ar" CFLAGS="${flags[*]}" >"$work/arm-make.log" 2>&1
  objects=()
  for source in "${sources[@]}"; do
    object="$work/$(basename "${source%.c}").o"
    "${ARM_PREFIX}gcc" "${flags[@]}" "${includes[@]}" -c "$source" -o "$object"
    objects+=("$object")
  done
  for variant in full combined tls ota; do
    defines=()
    [[ "$variant" != full ]] || defines=(-DDOWNLOAD_CORE)
    [[ "$variant" != tls ]] || defines=(-DTLS_ONLY)
    [[ "$variant" != ota ]] || defines=(-DOTA_ONLY)
    "${ARM_PREFIX}gcc" "${flags[@]}" "${includes[@]}" "${defines[@]}" -c tools/tls_probe/footprint.c -o "$work/footprint-$variant.o"
    "${ARM_PREFIX}gcc" "${flags[@]}" --specs=nano.specs --specs=nosys.specs -nostartfiles -Wl,--gc-sections,-e,_start,-Map,"$work/footprint-$variant.map" "$work/footprint-$variant.o" "${objects[@]}" "$src/arm/libbearssl.a" -o "$work/footprint-$variant.elf"
    "${ARM_PREFIX}size" "$work/footprint-$variant.elf"
  done
  "${ARM_PREFIX}nm" -S --size-sort "$work/footprint-combined.o" >"$work/arm-sizes.txt"
fi
