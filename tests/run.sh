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
    exec python3 -m unittest tests.test_native_status_arm tests.test_mqtt_discovery_budget.DiscoveryBudget.test_actual_arm_discovery_retry_exclusion_and_recovery
    ;;
  --legacy-sdk)
    exec bash tests/run_legacy_json_sdk.sh
    ;;
  --historical-product)
    exec bash tests/run_historical_product.sh --all
    ;;
  ""|--all) ;;
  *) echo 'Usage: tests/run.sh [--all|--native-common|--native-rc4|--native-arm|--legacy-sdk|--historical-product]' >&2; exit 2 ;;
esac
# Native behavior replaces the deleted device-DOM architecture only after an
# explicit scenario mapping in tests/coverage/rc4-native-migration.md.
source tests/host_hash.sh
# The original gate prepared every product SDK before MQTT fixtures. Native
# MQTT/storage now execute first, so establish those prerequisites before them.
export A1_SDK_DIR="${A1_SDK_DIR:-$root/build/a1-sdk/mico-os}"
export M1_SDK_DIR="${M1_SDK_DIR:-$root/build/m1-sdk/mico-os}"
export TC1_SDK_DIR="${TC1_SDK_DIR:-$root/build/tc1-sdk-3.6.4/mico-os}"
export PHICOMM_M1_SDK="${PHICOMM_M1_SDK:-$M1_SDK_DIR}"
export PHICOMM_TC1_SDK="${PHICOMM_TC1_SDK:-$TC1_SDK_DIR}"
for sdk in "$A1_SDK_DIR" "$M1_SDK_DIR" "$TC1_SDK_DIR"; do
    python3 tools/prepare_sdk.py "$sdk"
done
python3 -m tests.run_native_rc4
python3 -m unittest tests.test_prepare_sdk tests.test_config_commit \
  tests.test_apply_sdk_patch.ApplySdkPatchTests.test_ordered_stack_preserves_edits_and_rejects_partial_or_incompatible_state \
  tests.test_apply_sdk_patch.ApplySdkPatchTests.test_nested_archive_is_patched_and_idempotent \
  tests.test_apply_sdk_patch.ApplySdkPatchTests.test_sdk_git_repository_is_patched_and_idempotent \
  tests.test_apply_sdk_patch.ApplySdkPatchTests.test_plain_sdk_preserves_lf_when_patch_context_is_crlf
python3 -m unittest tests.test_version tests.test_package_stale_signature tests.test_mixed_candidate_completion tests.test_accepted_upload_postcheck
python3 -m unittest tests.test_readonly_stress tests.test_readonly_input_edges tests.test_readonly_shared_httpd tests.test_readonly_http_scheduling tests.test_readonly_parallel tests.test_summarize_readonly_evidence tests.test_audit_shared_candidate_completion
python3 -m unittest tests.test_http_core tests.test_http_core_tcp tests.test_shared_http_adversarial tests.test_shared_http_arm tests.test_shared_http_soak tests.test_shared_http_tcp_soak tests.test_http_mico_sdk_abi
python3 -m unittest tests.test_memory_admission.MemoryAdmission.test_budget_overlap_boundaries_and_wait_recovery tests.test_emw3080_release_memory tests.test_http_browser_queue tests.test_tc1_http_trace tests.test_a1_provision_memory
python3 -m unittest tests.test_host_rng tests.test_m1_barometer tests.test_m1_barometer_captures tests.test_numeric_feed tests.test_a1_filter_storage tests.test_m1_boot_config
python3 -m unittest tests.test_mqtt_stack_budget tests.test_mqtt_http_mixed tests.test_mixed_report tests.test_a1_mqtt_progress
# Actual TC1 SDK capture and admission, not the generic status oracle alone.
python3 -m unittest tests.test_typed_tc1_status tests.test_tc1_status_dispatch
python3 -m unittest tests.test_mqtt_discovery_budget.DiscoveryBudget.test_actual_arm_discovery_retry_exclusion_and_recovery
python3 -m unittest tests.test_ota_fetch
python3 -m unittest discover -s A1/tests -p test_fan_sensor.py
python3 -m unittest discover -s TC1/tests -p test_power_sensor.py
compiler=("${CC:-cc}")
if [[ "${SANITIZE:-0}" == 1 ]]; then
    compiler+=(-g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie)
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
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror tests/test_mqtt_account.c -o "$output/mqtt-account"
"$output/mqtt-account"
"${compiler[@]}" -std=c99 -Wall -Wextra -Werror -Itests/stubs -ITC1 tests/test_web_log.c -o "$output/web-log"
"$output/web-log"
for device in M1 A1 TC1; do
    "${compiler[@]}" -std=gnu99 -Wall -Wextra -Werror -DHOST_$device -Itests/host tests/test_migration.c -o "$output/migrate-$device"
    "$output/migrate-$device"
done
python3 tests/check_web.py
python3 tests/test_m1_clock.py
python3 -m unittest tests.test_site_docs tests.test_archive_pages
