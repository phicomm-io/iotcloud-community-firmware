#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/check_version.py
python3 -m unittest discover -s tests -p test_version.py
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
cc -std=c99 -Wall -Wextra -Werror tests/test_gesture.c -o "$output/gesture"
"$output/gesture"
cc -std=c99 -Wall -Wextra -Werror tests/test_wifi_trial.c -o "$output/wifi-trial"
"$output/wifi-trial"
cc -std=c99 -Wall -Wextra -Werror tests/test_m1_indicator.c -o "$output/m1-indicator"
"$output/m1-indicator"
cc -std=c99 -Wall -Wextra -Werror tests/test_a1_ring.c -o "$output/a1-ring"
"$output/a1-ring"
cc -DFEATURE_HOST -std=c99 -Wall -Wextra -Werror -Wno-misleading-indentation -Icommon common/engine.c tests/test_engine.c -lm -o "$output/engine"
cc -std=c99 -Wall -Wextra -Werror tests/test_mqtt_account.c -o "$output/mqtt-account"
"$output/mqtt-account"
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
cc -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Itests/stubs -I"$json_dir" common/service.c tests/test_service.c "$json_dir"/*.c -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -o "$output/service"
cc -std=gnu99 -DPHICOMM_OTA_CN_BASE_URL='"http://127.0.0.1:18766"' -Itests/stubs -I"$json_dir" tests/test_update.c "$json_dir"/*.c -o "$output/update"
python3 tests/test_update.py "$output/update"
cc -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -Icommon -I"$json_dir" tests/test_mqtt_service.c "$json_dir"/*.c -o "$output/mqtt-service"
"$output/mqtt-service"
"$output/service"
for define in -DFEATURE_DEVICE_A1 -DAUTOMATION_TC1; do
cc "$define" -std=gnu99 -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" common/service.c tests/test_automation.c "$json_dir"/*.c -o "$output/automation"
"$output/automation"
done
for device in M1 A1 TC1; do
    cc -std=gnu99 -Wall -Wextra -Werror -DHOST_$device -Itests/host -I"$json_dir" tests/test_migration.c -o "$output/migrate-$device"
    "$output/migrate-$device"
done
for device in A1 M1; do
    define=()
    if [[ "$device" == A1 ]]; then define=(-DFEATURE_DEVICE_A1); fi
    cc -std=gnu99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-misleading-indentation -Wno-format-truncation -Wno-sign-compare -Wno-implicit-fallthrough -Wno-nonnull-compare -Wno-unused-value -Itests/stubs -I"$json_dir" "${define[@]}" tests/test_runtime.c common/service.c "$json_dir"/*.c -lm -o "$output/runtime-$device"
    "$output/runtime-$device"
done
python3 tests/check_web.py
python3 tests/test_m1_clock.py
python3 -m unittest discover -s tests -p test_site_docs.py
