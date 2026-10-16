#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 tests/ota/test_signed_ota.py
OTA_SHA_DIR=build/a1-sdk/mico-os/MiCO/security/SHAUtils python3 tests/ota/test_signed_ota.py
json_dir=${PHICOMM_TEST_JSON_DIR:-build/a1-sdk/mico-os/libraries/utilities/json_c}
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
cp -R "$json_dir" "$output/json_c"
python3 - "$output/json_c" <<'PY'
import pathlib,sys
for p in pathlib.Path(sys.argv[1]).glob('*.c'):
    p.write_text(p.read_text().replace('%ld','%d'))
PY
cc -std=gnu99 -include stdint.h -Itests/stubs -Wno-unused-value -Wno-format -I"$output/json_c" tests/ota/test_mqtt_ota.c "$output/json_c"/*.c -o "$output/mqtt-ota"
"$output/mqtt-ota"

python3 tests/ota/test_tc1_adapter.py

python3 tests/ota/test_mico_adapter.py
