#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/simulator
compiler=("${CC:-cc}")
if [[ "${SANITIZE:-0}" == 1 ]]; then
 compiler+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
mkdir -p "$out/json"
cp -R build/r7-sdk/m1/libraries/utilities/json_c/. "$out/json/"
python3 - "$out/json" <<'PY'
import pathlib,sys
for p in pathlib.Path(sys.argv[1]).glob('*.c'): p.write_text(p.read_text().replace('%ld','%d'))
PY
for model in A1 M1; do
 sdk="build/r7-sdk/${model,,}/libraries/protocols/mqtt"
 extra=()
 if [[ $model == A1 ]]; then extra=(A1/control.c); else extra=(M1/mqtt_transport.c); fi
 "${compiler[@]}" -std=gnu99 -g -pthread -DHOST_$model -DMAX_MESSAGE_HANDLERS=12 -Itests/simulator -Itests/host -I"$model" -I"$out/json" -I"$sdk" -I"$sdk/MQTTPacket" tests/simulator/node.c "$model/features.c" "$model/service.c" "$model/mqtt.c" "${extra[@]}" tests/simulator/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c "$out/json"/*.c -lm -o "$out/$model"
done
