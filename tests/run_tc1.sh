#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sdk=build/tc1-sdk/mico-os/libraries/protocols/mqtt
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
cc -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-sign-compare -Wno-misleading-indentation -Wno-pointer-sign -Wno-implicit-fallthrough \
    -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" \
    tests/test_tc1_packet.c tests/host/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c -o "$output/tc1-packet"
"$output/tc1-packet"
python3 tests/test_tc1_http.py
python3 tests/test_tc1_gpio.py

python3 tests/test_tc1_core.py
