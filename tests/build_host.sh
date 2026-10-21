#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
source tests/host_hash.sh
output=${1:-build/account-host}
mkdir -p "$output"
json_sdk=$(mktemp -d)
trap 'rm -rf "$json_sdk"' EXIT
cc -std=c99 -Wall -Wextra -Werror tests/test_m1_scd40.c -o "$output/m1-scd40"
"$output/m1-scd40"
python3 -m unittest discover -s tests -p test_apply_sdk_patch.py
sdk_a1=${A1_SDK_DIR:-build/a1-sdk/mico-os}
sdk_m1=${M1_SDK_DIR:-build/m1-sdk/mico-os}
sdk_tc1=${TC1_SDK_DIR:-build/tc1-sdk-3.6.4/mico-os}
for model in A1 M1 TC1; do
    case "$model" in
        A1) sdk="$sdk_a1" ;;
        M1) sdk="$sdk_m1" ;;
        TC1) sdk="$sdk_tc1" ;;
    esac
    python3 tools/apply_sdk_patch.py "$sdk" common/mqtt-publish-bounds.patch
    mqtt="$sdk/libraries/protocols/mqtt"
    cc -std=gnu99 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$mqtt/MQTTPacket" tests/test_mqtt_publish_bounds.c \
        "$mqtt/MQTTPacket/MQTTDeserializePublish.c" \
        "$mqtt/MQTTPacket/MQTTPacket.c" -o "$output/$model-mqtt-publish-bounds"
    "$output/$model-mqtt-publish-bounds"
done
sdk="$sdk_m1/libraries/protocols/mqtt"
cc -std=gnu99 -Wall -Wextra -Werror -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" tests/test_m1_transport.c tests/host/network.c M1/mqtt_transport.c -o "$output/m1-transport"
"$output/m1-transport"
for model in m1 a1; do
    if [[ "$model" == m1 ]]; then sdk="$sdk_m1/libraries/protocols/mqtt"; else sdk="$sdk_a1/libraries/protocols/mqtt"; fi
cc -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-sign-compare -Wno-misleading-indentation -Wno-pointer-sign -Wno-implicit-fallthrough -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" tests/test_m1_packet.c tests/host/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c -o "$output/$model-packet"
"$output/$model-packet"
done
sdk="$sdk_tc1/libraries/protocols/mqtt"
cc -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-sign-compare -Wno-misleading-indentation -Wno-pointer-sign -Wno-implicit-fallthrough -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" tests/test_tc1_packet.c tests/host/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c -o "$output/tc1-packet"
"$output/tc1-packet"
for model in M1 A1 TC1; do
    case "$model" in
        A1) sdk_root="$sdk_a1" ;;
        M1) sdk_root="$sdk_m1" ;;
        TC1) sdk_root="$sdk_tc1" ;;
    esac
    sdk="$sdk_root/libraries/protocols/mqtt"
    json="$sdk_root/libraries/utilities/json_c"
    # CI prepares fresh SDKs independently of the firmware build jobs.
    # Patch a disposable copy so cached SDK contents are not a prerequisite.
    json_copy="$json_sdk/$model/libraries/utilities/json_c"
    mkdir -p "$json_copy"
    cp -R "$json/." "$json_copy/"
    python3 tools/apply_sdk_patch.py "$json_sdk/$model" \
        common/json-newlib-reent.patch common/json-printbuf-oom.patch \
        common/json-allocation-safety.patch common/json-heap-budget.patch \
        common/json-newlib-nano-abi.patch common/json-integer-parse.patch
    json="$json_copy"
    transport=()
    if [[ "$model" == M1 ]]; then transport=(M1/mqtt_transport.c); fi
    python3 - "$json" <<'PY'
import pathlib,sys
for p in pathlib.Path(sys.argv[1]).glob('*.c'):p.write_text(p.read_text().replace('%ld','%d'))
PY
    cc -std=gnu99 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-value -Wno-nonnull-compare -Wno-pointer-sign -DHOST_$model -DMAX_MESSAGE_HANDLERS=12 -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" -I"$json" -ITC1 -ITC1/mqtt_server "${hash_sources[@]}" tests/host/product.c common/service.c common/json_owned.c tests/host/network.c "${transport[@]}" "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c "$json"/*.c -lm -o "$output/$model"
    cc -std=gnu99 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-sign-compare -Wno-misleading-indentation -DHOST_$model -DMAX_MESSAGE_HANDLERS=12 -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" tests/test_suback.c tests/host/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c -o "$output/suback-$model"
    "$output/suback-$model"
done
