#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 tests/ota/test_signed_ota.py
OTA_SHA_DIR=build/a1-sdk/mico-os/MiCO/security/SHAUtils python3 tests/ota/test_signed_ota.py
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
cc -std=gnu99 -Werror=implicit-function-declaration -Itests/stubs tests/ota/test_mqtt_ota.c common/typed_wire.c common/wire_buffer.c -lm -o "$output/mqtt-ota"
"$output/mqtt-ota"

python3 tests/ota/test_tc1_adapter.py

python3 tests/ota/test_mico_adapter.py
