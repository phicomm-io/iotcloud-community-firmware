#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output=${1:-build/account-host}
mkdir -p "$output"
for model in M1 A1 TC1; do
    sdk="build/${model,,}-sdk/mico-os/libraries/protocols/mqtt"
    json="build/${model,,}-sdk/mico-os/libraries/utilities/json_c"
    mkdir -p "$output/json-$model"
    cp -R "$json/." "$output/json-$model/"
    python3 - "$output/json-$model" <<'PY'
import pathlib,sys
for p in pathlib.Path(sys.argv[1]).glob('*.c'):p.write_text(p.read_text().replace('%ld','%d'))
PY
    cc -std=gnu99 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-value -Wno-nonnull-compare -Wno-pointer-sign -DHOST_$model -DMAX_MESSAGE_HANDLERS=12 -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" -I"$output/json-$model" -ITC1 -ITC1/mqtt_server tests/host/product.c tests/host/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c "$output/json-$model"/*.c -lm -o "$output/$model"
    cc -std=gnu99 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-sign-compare -Wno-misleading-indentation -DHOST_$model -DMAX_MESSAGE_HANDLERS=12 -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" tests/test_suback.c tests/host/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c -o "$output/suback-$model"
    "$output/suback-$model"
done
