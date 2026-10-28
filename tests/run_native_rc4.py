"""Explicit RC4 native migration subset; not the complete historical gate.

Keep this list reviewable. The remaining product/tool gates run in run.sh --all.
Tests may use a pinned SDK/frozen legacy schema as an oracle, but production
business paths under test must be native MessagePack or the isolated HA writer.
"""
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULES = (
    'test_native_features', 'test_a1_fresh_air', 'test_runtime_session_mp',
    'test_json_integer_parse.JsonIntegerParseTest.test_m1_production_counter_body',
    'test_msgpack_core', 'test_status_msgpack', 'test_service_mp',
    'test_http_product_mp', 'test_http_callbacks', 'test_status_views',
    'test_http_mp_stack_gate', 'test_worker_mp_stack_gate',
    'test_local_auth', 'test_auth_routes_mp', 'test_storage_auth',
    'test_backup', 'test_m1_config_recovery', 'test_m1_storage_v10',
    'test_lan_wire', 'test_lan_client', 'test_group_mp', 'test_sensor_grant_mp', 'test_native_udp_runtime',
    'test_release_manifest_mp', 'test_mqtt_ota_mp', 'test_native_mqtt',
    'test_bemfa_mp', 'test_ha_json', 'test_ha_codec_shared', 'test_runtime_mqtt_mp',
    'test_sensor_producers_mp', 'test_sensor_mp', 'test_m1_display_mp',
    'test_tc1_native_sdk', 'test_no_json_objects',
    'test_shared_mqtt', 'test_mqtt_preflight_deferral', 'test_mqtt_birth_progress',
    'test_mqtt_buffer_storage', 'test_mqtt_subscribe_qos', 'test_tc1_mqtt_lifecycle', 'test_bemfa',
    'test_native_service_mqtt', 'test_tc1_mqtt_stack', 'test_tc1_checked_number_calls',
    'test_mqtt_optional_json',
    'test_mqtt_discovery_budget.DiscoveryBudget.test_host_discovery_lifecycle_sanitizers',
    'test_mqtt_discovery_budget.DiscoveryBudget.test_missing_capture_backoff_negative_control',
    'test_ha_runtime_contract', 'test_m1_mqtt_cleanup_callback',
    'test_product_metadata_mp', 'test_ota_recovery', 'test_stock_runtime',
    'test_m1_display_source', 'test_m1_display_format', 'test_m1_generic_fields',
    'test_tc1_status_guard',
    'test_m1_mqtt_display_session', 'test_m1_mqtt_startup', 'test_m1_particles_mqtt',
)

if __name__ == '__main__':
    subprocess.run([sys.executable, str(ROOT / 'tools/check_version.py')], check=True)
    print('RC4 native host regression: %u entries; retained gates additionally run in --all.' % len(MODULES), flush=True)
    suite = unittest.defaultTestLoader.loadTestsFromNames(['tests.' + m for m in MODULES])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.skipped:
        print('Native subset is incomplete: skipped cases are not accepted as PASS.', file=sys.stderr)
    raise SystemExit(0 if result.wasSuccessful() and not result.skipped else 1)
