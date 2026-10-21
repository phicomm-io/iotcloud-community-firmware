#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
model="$1"
compiler="$2"
work="$root/build/ota-tls"
mkdir -p "$work" "$root/$model/generated"
# All products share the source archive and ABI build directories.
exec 9>"$work/build.lock"
flock 9
if [[ ! -f "$work/bearssl-0.6/Makefile" ]]; then
  curl -fLsS --retry 3 https://bearssl.org/bearssl-0.6.tar.gz -o "$work/source.tar.gz"
  echo "6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14  $work/source.tar.gz" | sha256sum -c -
  tar -xzf "$work/source.tar.gz" -C "$work"
fi
abi=hard
flags="-Os -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
if [[ "$model" == TC1 ]]; then abi=soft; flags="-Os -mcpu=cortex-m4 -mthumb -mfloat-abi=soft"; fi
make -C "$work/bearssl-0.6" -j8 lib BUILD="arm-$abi" CC="${compiler}gcc" AR="${compiler}ar" CFLAGS="$flags -fno-common -ffunction-sections -fdata-sections" > "$work/build-$abi.log" 2>&1
cp "$work/bearssl-0.6/arm-$abi/libbearssl.a" "$root/$model/generated/libbearssl.a"
ln -sfn "$work/bearssl-0.6/inc" "$root/$model/generated/tls-include"
