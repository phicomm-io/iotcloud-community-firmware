"""Offline checks for the final mixed HTTP/MQTT evidence auditor."""
import base64
import json
import unittest

from tests.device import audit_mixed_candidate_completion as AUDIT


class SourceManifestTests(unittest.TestCase):
    def test_acceptance_manifest_requires_sampler_and_schema_comparator(self):
        names = AUDIT.REQUIRED_ACCEPTANCE_SOURCES
        failures = []
        self.assertEqual(AUDIT._require_acceptance_sources({name: {} for name in names}, failures), [])
        self.assertEqual(failures, [])
        incomplete = {name: {} for name in names}
        del incomplete['http_mqtt_schema_parity.py']
        failures = []
        self.assertEqual(AUDIT._require_acceptance_sources(incomplete, failures),
                         ['http_mqtt_schema_parity.py'])
        self.assertEqual(failures, ['required_acceptance_sources_missing'])


def load_runner(data):
    return {
        'memory': [
            {'ip': ip, 'phase': 'mixed-soak', 'ok': True, 'data': dict(data)}
            for ip in AUDIT.TARGETS
        ]
    }


class MixedLoadDiagnosticsTests(unittest.TestCase):
    def test_underload_diagnostics_check_budgets_and_owner_without_requiring_zero_queue(self):
        diag = {
            'mqtt_rx_queued': 2, 'mqtt_tx_queued': 3, 'mqtt_inflight': 2,
            'mqtt_queue_bytes': 5000, 'mqtt_credit_bytes': 2048,
            'mqtt_peak_bytes': 9000, 'mqtt_owner_faults': 0,
            'mqtt_rx_buffer_bytes': 512, 'mqtt_tx_buffer_bytes': 1024,
            'mqtt_errors': 1, 'mqtt_reconnects': 0,
            'mqtt_last_error_stage': 3, 'mqtt_last_error': -1,
            'mqtt_operation': 5, 'mqtt_protocol_depth': 1,
            'mqtt_max_queue_delay_ms': 50,
        }
        failures = []
        report = AUDIT._audit_load_diagnostics(load_runner(diag), failures, 'soak')
        self.assertEqual(failures, [])
        self.assertTrue(all(row['availability'] == 'available' for row in report.values()))
        self.assertTrue(all(row['violating_phases'] == [] for row in report.values()))
        self.assertEqual(report['10.117.152.111']['phases']['mixed-soak']['mqtt_errors_first'], 1)

    def test_underload_queue_byte_budget_and_owner_fault_are_blocking(self):
        diag = {
            'mqtt_rx_queued': 1, 'mqtt_tx_queued': 1, 'mqtt_inflight': 1,
            'mqtt_queue_bytes': 9000, 'mqtt_credit_bytes': 4000,
            'mqtt_peak_bytes': 13000, 'mqtt_owner_faults': 1,
            'mqtt_rx_buffer_bytes': 512, 'mqtt_tx_buffer_bytes': 1024,
        }
        failures = []
        report = AUDIT._audit_load_diagnostics(load_runner(diag), failures, 'soak')
        self.assertEqual(set(failures), {f'soak_load_diagnostic_bounds:{ip}' for ip in AUDIT.TARGETS})
        self.assertTrue(all(row['violating_phases'] == ['mixed-soak'] for row in report.values()))

    def test_underload_receive_queue_plus_inflight_limit_is_enforced(self):
        diag = {'mqtt_rx_queued': 3, 'mqtt_inflight': 2,
                'mqtt_tx_queued': 1, 'mqtt_queue_bytes': 1000,
                'mqtt_credit_bytes': 1000, 'mqtt_peak_bytes': 3000,
                'mqtt_owner_faults': 0}
        failures = []
        AUDIT._audit_load_diagnostics(load_runner(diag), failures, 'loadtest')
        self.assertEqual(set(failures), {f'loadtest_load_diagnostic_bounds:{ip}' for ip in AUDIT.TARGETS})

    def test_missing_underload_fields_are_explicitly_not_available(self):
        failures = []
        report = AUDIT._audit_load_diagnostics(load_runner({'free_memory': 28000}), failures, 'baseline')
        self.assertEqual(failures, [])
        for row in report.values():
            self.assertEqual(row['availability'], 'NOT_AVAILABLE')
            self.assertEqual(row['fields']['mqtt_queue_bytes'], 'NOT_AVAILABLE')
            self.assertEqual(row['samples'], 1)


class MQTTContractTests(unittest.TestCase):
    def test_exact_readonly_full_live_full_coverage_is_required(self):
        ident_model = {
            '10.117.152.111': ('b0f893303134', 'a1'),
            '10.117.152.102': ('D0BAE463E12C', 'tc1'),
            '10.117.152.110': ('b0f8932864b6', 'm1'),
        }
        rows = []
        for ip, (ident, model) in ident_model.items():
            for index, mode in enumerate(('full', 'live', 'full')):
                rows.append({
                    'ip': ip, 'model': model, 'mode': mode,
                    'request_id': f'readonly-mqtt-{ip.rsplit(".", 1)[1]}-{index}',
                    'action': 'status.get', 'params': {} if mode == 'full' else {'view': 'live'},
                    'retained': False, 'http_status': 200, 'response_code': 'ok',
                    'response_device_id': ident, 'response_model': model,
                    'response_version': '0.9.15-test',
                })
        evidence = {'status': 'complete', 'completed': True, 'version': '0.9.15-test',
                    'http_prechecks': 'passed', 'http_postchecks': 'passed',
                    'blockers': [], 'requests': rows}
        failures = []
        result = AUDIT._audit_readonly_mqtt(evidence, '0.9.15-test', failures)
        self.assertTrue(result['passed'])
        self.assertEqual(failures, [])
        evidence['requests'][1]['action'] = 'config.set'
        failures = []
        result = AUDIT._audit_readonly_mqtt(evidence, '0.9.15-test', failures)
        self.assertFalse(result['passed'])
        self.assertIn('mqtt_readonly_contract', failures)

    def test_http_full_live_bodies_are_real_successful_unique_and_identity_bound(self):
        samples = []
        before = {}
        for ip, (ident, model) in AUDIT.TARGETS.items():
            controls = {'a1': {'requested_fan_on': False, 'requested_speed': 0,
                               'child_lock': False, 'led_enabled': True},
                        'tc1': {'socket_states': [], 'child_lock': False, 'led_enabled': True},
                        'm1': {'brightness_requested': 20}}[model]
            before[ip] = {'boot_origin': 20.0, 'state': {
                'device_id': ident, 'model': model, 'version': '0.9.15-test',
                'uptime_seconds': 10, 'wifi_connected': True, 'mqtt_connected': True,
                **controls}}
            for view in ('full', 'live'):
                data = {'device_id': ident, 'model': model, 'version': '0.9.15-test',
                        'uptime_seconds': 12, 'wifi_connected': True, 'mqtt_connected': True,
                        **controls}
                raw = json.dumps({'code': 'ok', 'data': data}, separators=(',', ':')).encode()
                sample_index = len(samples)
                samples.append({'ip': ip, 'model': model, 'view': view,
                                'request_id': f'readonly-http-schema-{ip.rsplit(".", 1)[1]}-{view}',
                                'action': 'status.get',
                                'params': {} if view == 'full' else {'view': 'live'},
                                'http_status': 200, 'response_code': 'ok',
                                'response_device_id': ident, 'response_model': model,
                                'response_version': '0.9.15-test', 'body_bytes': len(raw),
                                'request_started_monotonic': 20 + sample_index * 2,
                                'request_ended_monotonic': 21 + sample_index * 2,
                                'raw_body_base64': base64.b64encode(raw).decode()})
        evidence = {'schema': 'candidate-http-status-bodies-v1', 'status': 'complete',
                    'completed': True, 'version': '0.9.15-test', 'blockers': [],
                    'targets': [{'ip': ip, 'device_id': ident, 'model': model}
                                for ip, (ident, model) in AUDIT.TARGETS.items()],
                    'samples': samples}
        failures = []
        result = AUDIT._audit_http_body_evidence(evidence, '0.9.15-test', failures,
                                                 {'before': before})
        self.assertTrue(result['passed'], failures)
        self.assertEqual(result['validated_bodies'], 6)
        self.assertEqual(failures, [])
        evidence['samples'][0]['params'] = {'view': 'live'}
        failures = []
        self.assertFalse(AUDIT._audit_http_body_evidence(
            evidence, '0.9.15-test', failures, {'before': before})['passed'])
        self.assertIn('http_schema_body_contract', failures)

    def test_http_body_audit_rejects_missing_timestamps_without_comparison_error(self):
        samples = []
        before = {}
        for ip, (ident, model) in AUDIT.TARGETS.items():
            controls = {'a1': {'requested_fan_on': False, 'requested_speed': 0,
                               'child_lock': False, 'led_enabled': True},
                        'tc1': {'socket_states': [], 'child_lock': False, 'led_enabled': True},
                        'm1': {'brightness_requested': 20}}[model]
            before[ip] = {'boot_origin': 20.0, 'state': {
                'device_id': ident, 'model': model, 'version': '0.9.15-test',
                'uptime_seconds': 10, 'wifi_connected': True, 'mqtt_connected': True,
                **controls}}
            for view in ('full', 'live'):
                data = {'device_id': ident, 'model': model, 'version': '0.9.15-test',
                        'uptime_seconds': 12, 'wifi_connected': True, 'mqtt_connected': True,
                        **controls}
                raw = json.dumps({'code': 'ok', 'data': data}, separators=(',', ':')).encode()
                sample = {'ip': ip, 'model': model, 'view': view,
                          'request_id': f'readonly-http-schema-{ip.rsplit(".", 1)[1]}-{view}',
                          'action': 'status.get',
                          'params': {} if view == 'full' else {'view': 'live'},
                          'http_status': 200, 'response_code': 'ok',
                          'response_device_id': ident, 'response_model': model,
                          'response_version': '0.9.15-test', 'body_bytes': len(raw),
                          'request_started_monotonic': 20 + len(samples) * 2,
                          'request_ended_monotonic': 21 + len(samples) * 2,
                          'raw_body_base64': base64.b64encode(raw).decode()}
                samples.append(sample)
        samples[0]['request_started_monotonic'] = None
        evidence = {'schema': 'candidate-http-status-bodies-v1', 'status': 'complete',
                    'completed': True, 'version': '0.9.15-test', 'blockers': [],
                    'targets': [{'ip': ip, 'device_id': ident, 'model': model}
                                for ip, (ident, model) in AUDIT.TARGETS.items()],
                    'samples': samples}
        failures = []
        result = AUDIT._audit_http_body_evidence(evidence, '0.9.15-test', failures,
                                                 {'before': before})
        self.assertFalse(result['passed'])
        self.assertIn('http_schema_body_contract', failures)

    def test_candidate_schema_version_or_response_count_mismatch_fails(self):
        groups = {model: {'full': {'response_count': 2}, 'live': {'response_count': 1}}
                  for model in ('a1', 'tc1', 'm1')}
        http_groups = {model: {'full': {'sample_count': 1}, 'live': {'sample_count': 1}}
                       for model in ('a1', 'tc1', 'm1')}
        parity = {f'{model}/full': {'status': 'compared', 'differences': []}
                  for model in ('a1', 'tc1', 'm1')}
        live_parity = {f'{model}/live': {'status': 'compared', 'differences': []}
                       for model in ('a1', 'tc1', 'm1')}
        report = {'final_contract': {
            'schema': 'http-mqtt-final-contract-v1',
            'candidate': {'expected_version': '0.9.15-test', 'version_match': True,
                          'all_response_versions_match': True},
            'sources': {
                'candidate_http_full': {'sample_counts': {'a1': 1, 'tc1': 1, 'm1': 1},
                                        'groups': {model: {'sample_count': 1} for model in http_groups}},
                'candidate_http_live': {'sample_counts': {'a1': 1, 'tc1': 1, 'm1': 1},
                                        'groups': {model: {'sample_count': 1} for model in http_groups}},
                'candidate_mqtt': {'issues': [], 'groups': groups},
            },
            'integrity_issues': [],
            'comparisons': {
                'candidate_http_full_vs_candidate_mqtt_full': parity,
                'candidate_http_live_vs_candidate_mqtt_live': live_parity,
                'historical_http_live_vs_candidate_http_live': {
                    f'{model}/live': {'status': 'unavailable_no_historical_http_live_body'}
                    for model in ('a1', 'tc1', 'm1')},
            },
        }}
        failures = []
        self.assertTrue(AUDIT._audit_schema_parity(report, '0.9.15-test', failures)['passed'])
        groups['tc1']['live']['response_count'] = 0
        failures = []
        self.assertFalse(AUDIT._audit_schema_parity(report, '0.9.15-test', failures)['passed'])
        self.assertIn('http_mqtt_schema_parity_integrity', failures)


if __name__ == '__main__':
    unittest.main()
