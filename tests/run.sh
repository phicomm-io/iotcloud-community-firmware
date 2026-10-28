#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
# Narrow migration entries are explicit subsets, never full-regression claims.
case "${1:-}" in
  --native-common)
    exec python3 -m unittest tests.test_native_features tests.test_runtime_mqtt_mp
    ;;
  --native-rc4)
    exec python3 -m tests.run_native_rc4
    ;;
  --native-arm)
    exec python3 -m unittest tests.test_native_status_arm
    ;;
  --legacy-sdk)
    exec bash tests/run_legacy_json_sdk.sh
    ;;
  ""|--all) ;;
  *) echo 'Usage: tests/run.sh [--all|--native-common|--native-rc4|--native-arm|--legacy-sdk]' >&2; exit 2 ;;
esac
# The background JSON scenarios also compile the production SHA-256 service.
source tests/host_hash.sh
python3 -m unittest tests.test_native_features tests.test_runtime_mqtt_mp tests.test_msgpack_core tests.test_status_msgpack tests.test_service_mp tests.test_release_manifest_mp tests.test_http_product_mp tests.test_mqtt_ota_mp tests.test_http_mp_stack_gate tests.test_worker_mp_stack_gate
python3 -m unittest tests.test_prepare_sdk tests.test_apply_sdk_patch
python3 -m unittest tests.test_config_commit
python3 tools/check_version.py
python3 -m unittest discover -s tests -p test_version.py
python3 -m unittest tests.test_package_stale_signature
python3 -m unittest tests.test_mixed_candidate_completion
python3 -m unittest tests.test_accepted_upload_postcheck
python3 -m unittest tests.test_readonly_stress tests.test_readonly_input_edges tests.test_readonly_shared_httpd tests.test_readonly_http_scheduling tests.test_readonly_parallel tests.test_summarize_readonly_evidence tests.test_audit_shared_candidate_completion
python3 -m unittest tests.test_http_core tests.test_http_core_tcp tests.test_shared_http_adversarial tests.test_shared_http_arm tests.test_shared_http_soak tests.test_shared_http_tcp_soak tests.test_http_mico_sdk_abi
python3 -m unittest tests.test_json_build_gate tests.test_service_json_failure_scope tests.test_http_json_owned tests.test_http_json_budget tests.test_status_bool tests.test_mqtt_discovery_budget tests.test_background_json tests.test_tc1_mqtt_stack tests.test_tc1_checked_number_calls tests.test_ota_recovery
python3 -m unittest tests.test_memory_admission tests.test_emw3080_release_memory tests.test_http_browser_queue tests.test_tc1_http_trace
python3 -m unittest tests.test_a1_provision_memory
python3 -m unittest tests.test_info_metadata tests.test_m1_mqtt_display_session tests.test_host_rng tests.test_capabilities_wire
python3 -m unittest tests.test_m1_barometer tests.test_m1_barometer_captures
compiler=("${CC:-cc}")
if [[ "${SANITIZE:-0}" == 1 ]]; then
    compiler+=(-g -fno-omit-frame-pointer -fsanitize=address,undefined)
fi
compiler+=("${hash_sources[@]}")
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_stock_protocol.c M1/protocol.c -o "$output/m1-stock-protocol"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_m1_sensor_details.c -o "$output/m1-sensor-details"
"$output/m1-sensor-details"
"$output/m1-stock-protocol"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_ota_version.c -o "$output/ota-version"
"$output/ota-version"
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
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror -Wno-misleading-indentation tests/test_a1_fresh_air.c -lm -o "$output/a1-fresh-air"
"$output/a1-fresh-air"
python3 -m unittest tests.test_a1_fresh_air
python3 -m unittest tests.test_numeric_feed
python3 -m unittest discover -s A1/tests -p test_fan_sensor.py
python3 -m unittest discover -s TC1/tests -p test_power_sensor.py
python3 -m unittest tests.test_a1_filter_storage tests.test_status_views tests.test_status_direct tests.test_typed_tc1_status tests.test_tc1_status_guard tests.test_tc1_status_dispatch
"${compiler[@]}" -DFEATURE_HOST -std=c99 -Wall -Wextra -Werror -Wno-misleading-indentation -Icommon common/engine.c tests/test_engine.c -lm -o "$output/engine"
"${compiler[@]}" -DFEATURE_HOST -std=c99 -Wall -Wextra -Werror -Wno-misleading-indentation -Icommon common/engine.c tests/test_sensor_aggregate.c -lm -o "$output/sensor-aggregate"
"$output/sensor-aggregate"
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
    common/json-newlib-nano-abi.patch common/json-integer-parse.patch \
    common/json-sized-resize.patch \
    common/json-number-cache-cleanup.patch \
    common/json-number-checked.patch
python3 tools/install_json_numeric.py "$output/sdk"
ln -s sdk/libraries/utilities/json_c "$output/json_c"
python3 - "$output/json_c" <<'PYHOST'
import pathlib, sys
for p in pathlib.Path(sys.argv[1]).glob('*.c'):
    p.write_text(p.read_text().replace('%ld', '%d'))
PYHOST
json_dir="$output/json_c"
# Product callbacks need the actual JSON lifetime overlays, including their
# headers. CI starts with a pristine SDK, so run after preparing this copy.
PHICOMM_TEST_JSON_DIR="$json_dir" python3 -m unittest tests.test_json_integer_parse tests.test_json_sized_resize tests.test_http_callbacks
# M1's production display encoder also uses the patched printbuf failure flag.
# Pass the same disposable SDK copy as the callbacks rather than pristine headers.
PHICOMM_TEST_JSON_DIR="$json_dir" python3 -m unittest tests.test_m1_boot_config tests.test_m1_config_recovery tests.test_stock_runtime tests.test_m1_display_source tests.test_m1_display_format tests.test_m1_generic_fields tests.test_m1_storage_v10 tests.test_backup tests.test_m1_particles_mqtt
# Production MQTT tests require every pinned SDK; never silently skip in CI.
for sdk in build/a1-sdk/mico-os build/m1-sdk/mico-os build/tc1-sdk-3.6.4/mico-os; do
    python3 tools/prepare_sdk.py "$sdk"
done
python3 -m unittest tests.test_json_number_cache tests.test_json_numeric_checked tests.test_json_numeric_stack tests.test_json_numeric_link tests.test_json_float32
PHICOMM_TEST_JSON_DIR="$json_dir" python3 -m unittest tests.test_bemfa tests.test_mqtt_stack_budget tests.test_mqtt_optional_json tests.test_shared_mqtt tests.test_mqtt_buffer_storage tests.test_mqtt_preflight_deferral tests.test_mqtt_http_mixed tests.test_mixed_report tests.test_a1_mqtt_progress tests.test_mqtt_birth_progress tests.test_tc1_mqtt_lifecycle tests.test_m1_mqtt_startup
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -DWATCHDOG_HOST -Itests/watchdog -Itests/stubs -I"$json_dir" tests/test_watchdog.c "$json_dir"/*.c -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -o "$output/watchdog"
"$output/watchdog"
# Original portable/MOC reset/heap/wrap assertions now run native MP through
# test_native_features; do not relink the historical JSON library into this test.

"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" tests/test_json_printbuf_failure.c "$json_dir"/*.c -Wl,--wrap=json_heap_resize -o "$output/json-printbuf-failure"
"$output/json-printbuf-failure"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" tests/test_json_allocation_failure.c "$json_dir"/*.c -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=strdup -o "$output/json-allocation-failure"
"$output/json-allocation-failure"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" -pthread tests/test_json_heap.c "$json_dir"/*.c -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=strdup -Wl,--wrap=free -o "$output/json-heap"
"$output/json-heap"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Itests/stubs -I"$json_dir" common/service.c tests/test_service.c common/json_owned.c "$json_dir"/*.c -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -o "$output/service"
ota_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
export PHICOMM_TEST_OTA_PORT="$ota_port"
"${compiler[@]}" -std=gnu99 -DPHICOMM_OTA_CN_BASE_URL='"http://127.0.0.1:'"$ota_port"'"' -Itests/stubs -I"$json_dir" tests/test_update.c common/json_owned.c "$json_dir"/*.c -o "$output/update"
python3 tests/test_update.py "$output/update"
"${compiler[@]}" -std=gnu99 -Itests/stubs -I"$json_dir" tests/test_device_update_version.c common/json_owned.c "$json_dir"/*.c -o "$output/device-update-version"
"$output/device-update-version"
# Isolated lifetime vectors use the exact shared production helper bodies.
python3 - "$output/mqtt_owned_helpers.inc" <<'PYMQTT'
from pathlib import Path
from tests.test_status_views import function
source=Path('common/mqtt/service.c').read_text()
Path(__import__('sys').argv[1]).write_text('\n'.join(function(source,n) for n in
    ('static struct printbuf *service_mqtt_wire(', 'int service_mqtt_send(')))
PYMQTT
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -I"$output" -Itests/stubs -Icommon -I"$json_dir" tests/test_mqtt_service.c common/json_owned.c "$json_dir"/*.c -Wl,--wrap=free -Wl,--wrap=json_heap_resize -o "$output/mqtt-service"
"$output/mqtt-service"
"$output/service"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" tests/test_service_failure.c common/json_owned.c "$json_dir"/*.c -o "$output/service-failure"
"$output/service-failure"
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir" tests/test_service_memory.c common/json_owned.c "$json_dir"/*.c -o "$output/service-memory"
"$output/service-memory"
# Full A1/M1/TC1 scheduler/button/countdown cases in test_automation.c now
# execute automation_mp via test_native_features above; no JSON service link.
"${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-value -Wno-nonnull-compare -Itests/host -I"$json_dir" tests/test_a1_control.c "$json_dir"/*.c -o "$output/a1-control"
"$output/a1-control"
for device in M1 A1 TC1; do
    "${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -DHOST_$device -Itests/host -I"$json_dir" tests/test_migration.c -o "$output/migrate-$device"
    "$output/migrate-$device"
done
for device in A1 M1; do
    define=()
    if [[ "$device" == A1 ]]; then define=(-DFEATURE_DEVICE_A1); fi
    "${compiler[@]}" -std=gnu99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-misleading-indentation -Wno-format-truncation -Wno-sign-compare -Wno-implicit-fallthrough -Wno-nonnull-compare -Wno-unused-value -Itests/stubs -I"$json_dir" "${define[@]}" tests/test_runtime.c common/service.c common/json_owned.c "$json_dir"/*.c -lm -o "$output/runtime-$device"
    "$output/runtime-$device"
done
python3 tests/check_web.py
python3 tests/test_m1_clock.py
python3 -m unittest discover -s tests -p test_site_docs.py

python3 -m unittest discover -s tests -p test_archive_pages.py
