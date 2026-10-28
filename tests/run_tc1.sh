#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tc1_sdk=${TC1_SDK_DIR:-build/tc1-sdk-3.6.4/mico-os}
python3 tools/apply_sdk_patch.py "$tc1_sdk" \
    common/mqtt-mico36.patch common/mqtt-publish-bounds.patch \
    common/mqtt-socket-close.patch M1/mqtt-packet-deadline.patch \
    common/mqtt-response-deadline.patch common/mqtt-buffer-storage.patch
sdk=$tc1_sdk/libraries/protocols/mqtt
export PHICOMM_TC1_SDK="$tc1_sdk"
python3 -m unittest tests.test_native_mqtt tests.test_ha_json tests.test_sensor_producers_mp tests.test_tc1_http_trace tests.test_bemfa_mp tests.test_mqtt_stack_budget
# This job builds only TC1; shared product callbacks must use its prepared
# pinned JSON library instead of silently depending on an A1 build tree.
export PHICOMM_TEST_JSON_DIR=${PHICOMM_TEST_JSON_DIR:-$tc1_sdk/libraries/utilities/json_c}
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
cc -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-sign-compare -Wno-misleading-indentation -Wno-pointer-sign -Wno-implicit-fallthrough \
    -Itests/host -I"$sdk" -I"$sdk/MQTTPacket" \
    tests/test_tc1_packet.c tests/host/network.c "$sdk/MQTTClient.c" "$sdk"/MQTTPacket/*.c -o "$output/tc1-packet"
"$output/tc1-packet"
python3 -m unittest tests.test_http_core tests.test_http_core_tcp tests.test_http_callbacks tests.test_m1_mcu_bulk_ota tests.test_shared_http_adversarial tests.test_shared_http_arm
python3 -m unittest tests.test_json_numeric_link tests.test_json_newlib_nano tests.test_json_integer_parse
JSON_NUMBER_CACHE_MODELS=TC1 python3 -m unittest tests.test_json_number_cache
JSON_NUMERIC_MODELS=TC1 python3 -m unittest tests.test_json_numeric_checked
python3 -m unittest tests.test_tc1_mqtt_stack tests.test_tc1_mqtt_config tests.test_tc1_checked_number_calls
python3 tests/test_tc1_gpio.py
python3 -m unittest tests.test_tc1_buttons

python3 tests/test_tc1_core.py

python3 tests/test_tc1_wifi.py

python3 -m unittest tests.test_tc1_sdk_migration tests.test_mico_network_close
