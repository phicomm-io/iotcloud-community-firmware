#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/check_version.py
python3 -m unittest discover -s tests -p test_version.py
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
cc -DFEATURE_HOST -std=c99 -Wall -Wextra -Werror -Wno-misleading-indentation -Icommon common/engine.c tests/test_engine.c -lm -o "$output/engine"
"$output/engine"
json_dir=${PHICOMM_TEST_JSON_DIR:-build/a1-sdk/mico-os/libraries/utilities/json_c}
if [[ ! -f "$json_dir/json.h" ]]; then echo 'Set PHICOMM_TEST_JSON_DIR to the pinned MiCO json_c directory' >&2; exit 1; fi
# MiCO's json-c assumes 32-bit long. Adapt its printf/scanf formats only in
# a disposable host copy, leaving firmware sources byte-for-byte untouched.
cp -R "$json_dir" "$output/json_c"
python3 - "$output/json_c" <<'PYHOST'
import pathlib, sys
for p in pathlib.Path(sys.argv[1]).glob('*.c'):
    p.write_text(p.read_text().replace('%ld', '%d'))
PYHOST
json_dir="$output/json_c"
for device in A1 M1; do
    define=()
    if [[ "$device" == A1 ]]; then define=(-DFEATURE_DEVICE_A1); fi
    cc -std=gnu99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-misleading-indentation -Wno-format-truncation -Wno-sign-compare -Wno-implicit-fallthrough -Wno-nonnull-compare -Wno-unused-value -Itests/stubs -I"$json_dir" "${define[@]}" tests/test_runtime.c "$json_dir"/*.c -lm -o "$output/runtime-$device"
    "$output/runtime-$device"
done
python3 tests/check_web.py
