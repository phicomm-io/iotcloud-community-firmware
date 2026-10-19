#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sdk=${TC1_SDK_DIR:-build/tc1-sdk-3.6.4/mico-os}/libraries/protocols/mqtt
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
cc -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-sign-compare -Wno-misleading-indentation -Wno-pointer-sign -Wno-implicit-fallthrough \
    -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" \
    tests/test_tc1_packet.c tests/host/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c -o "$output/tc1-packet"
"$output/tc1-packet"
python3 tests/test_tc1_http.py
python3 -m unittest tests.test_json_newlib_nano
python3 tests/test_tc1_gpio.py

python3 tests/test_tc1_core.py

python3 tests/test_tc1_wifi.py

python3 -m unittest tests.test_tc1_sdk_migration tests.test_tc1_binary_upload tests.test_tc1_sdk_auth tests.test_mico_network_close
