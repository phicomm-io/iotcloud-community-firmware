#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -m unittest tests.test_prepare_sdk tests.test_apply_sdk_patch
python3 tools/check_version.py
python3 -m unittest discover -s tests -p test_version.py
python3 -m unittest tests.test_package_stale_signature
python3 -m unittest tests.test_readonly_stress tests.test_readonly_input_edges tests.test_readonly_shared_httpd tests.test_readonly_http_scheduling tests.test_summarize_readonly_evidence
python3 -m unittest tests.test_http_core tests.test_http_core_tcp tests.test_shared_http_adversarial tests.test_shared_http_arm tests.test_shared_http_soak tests.test_shared_http_tcp_soak tests.test_http_mico_sdk_abi
python3 -m unittest tests.test_http_json_owned tests.test_http_json_budget tests.test_tc1_mqtt_stack
python3 -m unittest tests.test_memory_admission tests.test_http_browser_queue tests.test_tc1_http_trace
python3 -m unittest tests.test_a1_provision_memory
python3 -m unittest tests.test_m1_boot_config
compiler=("${CC:-cc}")
if [[ "${SANITIZE:-0}" == 1 ]]; then
    compiler+=(-g -fno-omit-frame-pointer -fsanitize=address,undefined)
fi
root="$PWD"
source tests/host_hash.sh
compiler+=("${hash_sources[@]}")
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_gesture.c -o "$output/gesture"
"$output/gesture"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_a1_buttons.c -o "$output/a1-buttons"
"$output/a1-buttons"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_a1_light.c -o "$output/a1-light"
"$output/a1-light"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_wifi_trial.c -o "$output/wifi-trial"
"$output/wifi-trial"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_m1_indicator.c -o "$output/m1-indicator"
"$output/m1-indicator"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_a1_ring.c -o "$output/a1-ring"
"$output/a1-ring"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_a1_filter.c -o "$output/a1-filter"
"$output/a1-filter"
python3 -m unittest tests.test_a1_filter_storage tests.test_status_views
"${compiler[@]}" -DFEATURE_HOST -std=c99 -Wall -Wextra -Werror -Wno-misleading-indentation -Icommon common/engine.c tests/test_engine.c -lm -o "$output/engine"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_mqtt_account.c -o "$output/mqtt-account"
"$output/mqtt-account"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror -Itests/stubs -ITC1 tests/test_web_log.c -o "$output/web-log"
"$output/web-log"
"$output/engine"
json_dir=${PHICOMM_TEST_JSON_DIR:-build/a1-sdk/mico-os/libraries/utilities/json_c}
if [[ ! -f "$json_dir/json.h" ]]; then echo 'Set PHICOMM_TEST_JSON_DIR to the pinned MiCO json_c directory' >&2; exit 1; fi
# MiCO's json-c assumes 32-bit long. Adapt its printf/scanf formats only in
# a disposable host copy, leaving firmware sources byte-for-byte untouched.
mkdir -p "$output/sdk/libraries/utilities"
cp -R "$json_dir" "$output/sdk/libraries/utilities/json_c"
python3 tools/apply_sdk_patch.py "$output/sdk" \
    common/json-newlib-reent.patch common/json-printbuf-oom.patch \
    common/json-allocation-safety.patch common/json-heap-budget.patch \
    common/json-newlib-nano-abi.patch common/json-integer-parse.patch
ln -s sdk/libraries/utilities/json_c "$output/json_c"
python3 - "$output/json_c" <<'PYHOST'
import pathlib, sys
for p in pathlib.Path(sys.argv[1]).glob('*.c'):
    p.write_text(p.read_text().replace('%ld', '%d'))
PYHOST
json_dir="$output/json_c"
# Product callbacks need the actual JSON lifetime overlays, including their
# headers. CI starts with a pristine SDK, so run after preparing this copy.
PHICOMM_TEST_JSON_DIR="$json_dir" python3 -m unittest tests.test_json_integer_parse tests.test_http_callbacks
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -DWATCHDOG_HOST -Itests/watchdog -Itests/stubs -I"$json_dir" tests/test_watchdog.c "$json_dir"/*.c -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -o "$output/watchdog"
"$output/watchdog"
for target in portable moc-mx1290; do
    target_defines=()
    if [[ "$target" == moc-mx1290 ]]; then target_defines=(-DMOC=1 -DCONFIG_CPU_MX1290); fi
    "${compiler[@]}" "${target_defines[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" tests/test_runtime_metrics.c "$json_dir"/*.c -o "$output/runtime-metrics-$target"
    "$output/runtime-metrics-$target"
done
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" tests/test_json_printbuf_failure.c "$json_dir"/*.c -Wl,--wrap=realloc -o "$output/json-printbuf-failure"
"$output/json-printbuf-failure"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" tests/test_json_allocation_failure.c "$json_dir"/*.c -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=strdup -o "$output/json-allocation-failure"
"$output/json-allocation-failure"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" -pthread tests/test_json_heap.c "$json_dir"/*.c -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=strdup -Wl,--wrap=free -o "$output/json-heap"
"$output/json-heap"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Itests/stubs -I"$json_dir" common/service.c tests/test_service.c "$json_dir"/*.c -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -o "$output/service"
ota_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
export PHICOMM_TEST_OTA_PORT="$ota_port"
"${compiler[@]}" -std=gnu99 -DPHICOMM_OTA_CN_BASE_URL='"http://127.0.0.1:'"$ota_port"'"' -Itests/stubs -I"$json_dir" tests/test_update.c "$json_dir"/*.c -o "$output/update"
python3 tests/test_update.py "$output/update"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -Icommon -I"$json_dir" tests/test_mqtt_service.c common/json_owned.c "$json_dir"/*.c -Wl,--wrap=free -Wl,--wrap=realloc -o "$output/mqtt-service"
"$output/mqtt-service"
"$output/service"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" tests/test_service_failure.c "$json_dir"/*.c -o "$output/service-failure"
"$output/service-failure"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" tests/test_service_memory.c "$json_dir"/*.c -o "$output/service-memory"
"$output/service-memory"
for define in -DFEATURE_DEVICE_A1 -DAUTOMATION_TC1; do
"${compiler[@]}" "$define" -std=gnu99 -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" common/service.c tests/test_automation.c "$json_dir"/*.c -o "$output/automation"
"$output/automation"
done
for device in M1 A1 TC1; do
    "${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -DHOST_$device -Itests/host -I"$json_dir" tests/test_migration.c -o "$output/migrate-$device"
    "$output/migrate-$device"
done
for device in A1 M1; do
    define=()
    if [[ "$device" == A1 ]]; then define=(-DFEATURE_DEVICE_A1); fi
    "${compiler[@]}" -std=gnu99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-misleading-indentation -Wno-format-truncation -Wno-sign-compare -Wno-implicit-fallthrough -Wno-nonnull-compare -Wno-unused-value -Itests/stubs -I"$json_dir" "${define[@]}" tests/test_runtime.c common/service.c "$json_dir"/*.c -lm -o "$output/runtime-$device"
    "$output/runtime-$device"
done
python3 tests/check_web.py
python3 tests/test_m1_clock.py
python3 -m unittest discover -s tests -p test_site_docs.py

python3 -m unittest discover -s tests -p test_archive_pages.py
