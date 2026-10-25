#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/simulator
selected=$(python3 tests/host_models.py)
mapfile -t models <<< "$selected"
compiler=("${CC:-cc}")
if [[ "${SANITIZE:-0}" == 1 ]]; then
 compiler+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
root="$PWD"
source tests/host_hash.sh
compiler+=("${hash_sources[@]}")
mkdir -p "$out"
json_sdk=$(mktemp -d)
trap 'rm -rf "$json_sdk"' EXIT
json="$json_sdk/libraries/utilities/json_c"
mkdir -p "$json"
sdk_a1=${A1_SDK_DIR:-build/a1-sdk/mico-os}
sdk_m1=${M1_SDK_DIR:-build/m1-sdk/mico-os}
json_source=
for model in "${models[@]}"; do
 if [[ "$model" == A1 || "$model" == M1 ]]; then
  if [[ "$model" == A1 ]]; then json_source="$sdk_a1"; else json_source="$sdk_m1"; fi
  break
 fi
done
if [[ -z "$json_source" ]]; then
 echo 'NOT_RUN simulator build: no selected model has simulator support (A1 or M1)' >&2
 exit 0
fi
cp -R "$json_source/libraries/utilities/json_c/." "$json/"
python3 tools/apply_sdk_patch.py "$json_sdk" \
 common/json-newlib-reent.patch common/json-printbuf-oom.patch \
 common/json-allocation-safety.patch common/json-heap-budget.patch \
 common/json-newlib-nano-abi.patch common/json-integer-parse.patch \
 common/json-sized-resize.patch \
    common/json-number-cache-cleanup.patch \
    common/json-number-checked.patch
    python3 tools/install_json_numeric.py "$json_sdk"
python3 - "$json" <<'PY'
import pathlib,sys
for p in pathlib.Path(sys.argv[1]).glob('*.c'): p.write_text(p.read_text().replace('%ld','%d'))
PY
for model in "${models[@]}"; do
 if [[ "$model" != A1 && "$model" != M1 ]]; then continue; fi
 if [[ $model == A1 ]]; then sdk="$sdk_a1"; else sdk="$sdk_m1"; fi
 # Match production builds: bound A1 needs more than five subscription handlers.
 python3 tools/apply_sdk_patch.py "$sdk" "$model/features-sdk.patch"
 python3 tools/apply_sdk_patch.py "$sdk" \
  common/mqtt-mico36.patch common/mqtt-publish-bounds.patch \
  common/mqtt-socket-close.patch M1/mqtt-packet-deadline.patch \
  common/mqtt-response-deadline.patch common/mqtt-buffer-storage.patch
 sdk="$sdk/libraries/protocols/mqtt"
 extra=()
 if [[ $model == A1 ]]; then extra=(A1/control.c); else extra=(M1/mqtt_transport.c M1/co2_source.c M1/display_source.c); fi
 "${compiler[@]}" -std=gnu99 -g -pthread -DHOST_$model -DMAX_MESSAGE_HANDLERS=12 -Itests/simulator -Itests/host -I"$model" -I"$json" -I"$sdk" -I"$sdk/mico" -I"$sdk/MQTTPacket" tests/simulator/node.c "$model/features.c" "$model/service.c" "$model/mqtt.c" common/json_owned.c common/mqtt/{core,client,session,service,diagnostics}.c "${extra[@]}" tests/simulator/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c "$json"/*.c -lm -o "$out/$model"
done
