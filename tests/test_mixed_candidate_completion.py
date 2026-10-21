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


def quiet_runner(data):
    memory = []
    for ip in AUDIT.TARGETS:
        for phase in ('baseline', 'final'):
            memory.extend({'ip': ip, 'phase': phase, 'ok': True, 'http': 200,
                           'data': dict(data)} for _ in range(3))
    return {'memory': memory}


def quiet_diagnostics():
    return {
        'json_live_blocks': 0, 'memory_active': 0, 'memory_reserved': 0,
        'memory_waiting': 0,
        'http_core_owner_state': 1, 'http_core_close_failures': 0,
        'http_core_restarts': 0, 'http_core_tcp_rc': 0, 'http_core_occupied': 0,
        'mqtt_rx_queued': 0, 'mqtt_tx_queued': 0, 'mqtt_inflight': 0,
        'mqtt_queue_bytes': 0, 'mqtt_credit_bytes': 0, 'mqtt_peak_bytes': 0,
        'mqtt_owner_faults': 0, 'mqtt_generation': 1, 'mqtt_rejected': 0,
        'mqtt_rx_buffer_bytes': 512, 'mqtt_tx_buffer_bytes': 512,
        'mqtt_protocol_depth': 0, 'mqtt_operation': 1,
        'mqtt_last_error_stage': 1, 'mqtt_last_error': 0, 'mqtt_errors': 0,
        'mqtt_reconnects': 0, 'mqtt_max_queue_delay_ms': 0,
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

    def test_application_mqtt_stages_pass_both_diagnostic_audits(self):
        for field in ('mqtt_operation', 'mqtt_last_error_stage'):
            for stage in range(8, 15):
                with self.subTest(field=field, stage=stage):
                    diag = quiet_diagnostics()
                    diag[field] = stage
                    failures = []
                    report = AUDIT._audit_load_diagnostics(load_runner(diag), failures, 'stages')
                    self.assertEqual(failures, [])
                    self.assertTrue(all(row['violating_phases'] == []
                                        for row in report.values()))
                    failures = []
                    result = AUDIT._quiet_resources(quiet_runner(diag), '0.9.15-test',
                                                    failures, 'stages', require_queue=True)
                    self.assertEqual(failures, [])
                    self.assertTrue(all(row['passed'] for row in result.values()))

    def test_out_of_range_or_malformed_mqtt_stages_fail_both_diagnostic_audits(self):
        for field in ('mqtt_operation', 'mqtt_last_error_stage'):
            for value in (15, -1, True, '8'):
                with self.subTest(field=field, value=value):
                    diag = quiet_diagnostics()
                    diag[field] = value
                    failures = []
                    report = AUDIT._audit_load_diagnostics(load_runner(diag), failures, 'stages')
                    self.assertEqual(set(failures), {
                        f'stages_load_diagnostic_bounds:{ip}' for ip in AUDIT.TARGETS})
                    self.assertTrue(all(row['violating_phases'] == ['mixed-soak']
                                        for row in report.values()))
                    failures = []
                    result = AUDIT._quiet_resources(quiet_runner(diag), '0.9.15-test',
                                                    failures, 'stages', require_queue=True)
                    self.assertEqual(set(failures), {
                        f'stages_quiet_resources:{ip}' for ip in AUDIT.TARGETS})
                    self.assertTrue(all(not row['passed'] for row in result.values()))


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



class InitialReuseTests(unittest.TestCase):
    """Synthetic evidence, real P-256 signatures with an ephemeral trusted test key.

    No sockets, DUT, broker, or ignored production artifacts are required.
    """
    def setUp(self):
        import tempfile
        from pathlib import Path
        from unittest import mock
        from cryptography.hazmat.primitives.asymmetric import ec
        from tools.ota import package
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.private = self.base/'private'
        self.label, self.archive = 'old-r2', 'candidate'
        self.commit, self.version = 'a'*40, '0.9.15-aaaaaa'
        for name, value in (('BASE', self.base), ('PRIVATE', self.private)):
            patch = mock.patch.object(AUDIT, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        self.key = ec.generate_private_key(ec.SECP256R1())
        patch = mock.patch.object(package, 'trusted_public_key', return_value=self.key.public_key())
        patch.start()
        self.addCleanup(patch.stop)
        self.outer_dir = self.private/(self.label+'-mixed-final-controller')
        self.inner_dir = self.private/(self.label+'-initial-controller')
        self.outer_dir.mkdir(parents=True)
        self.inner_dir.mkdir(parents=True)
        self.paced_path = self.private/(self.label+'-paced-diagnostics')/'evidence.json'
        self.paced = self.runner()
        summary = {}
        for ip in AUDIT.TARGETS:
            for phase, count in (('paced-full', 90), ('paced-live', 30)):
                summary[ip+' '+phase] = {'requests': count, 'http_200_ok': count,
                                         'http_503': 0, 'no_http': 0, 'other_http': 0}
                self.paced['rows'].extend({'ip': ip, 'phase': phase, 'http': 200,
                                           'code': 'ok'} for _ in range(count))
        self.write(self.paced_path, self.paced)
        files, deployed = {}, {}
        for model in ('a1', 'tc1', 'm1'):
            ota = 'phicomm_'+('tc1_a1' if model == 'tc1' else model)+'.ota.bin'
            raw = (model+' firmware').encode()
            signed = package.sign(raw, model, '0.9.15', self.commit, self.key)
            manifest = {'git_commit': self.commit, 'version': self.version, 'dirty': False,
                        'base_version': '0.9.15', 'files': {ota: self.digest(raw)},
                        'target': {'a1': 'A1@MK3080B@moc', 'm1': 'M1@MK3080B@moc',
                                   'tc1': 'TC1@MK3031@moc'}[model]}
            folder = self.base/'candidates'/self.archive/model
            self.write(folder/'manifest.json', manifest)
            (folder/ota).write_bytes(raw)
            (folder/(ota+'.signed')).write_bytes(signed)
            (folder/'SHA256SUMS').write_text(self.digest(raw)+'  '+ota+'\n')
            for name in ('manifest.json', 'SHA256SUMS', ota, ota+'.signed'):
                files[model+'/'+name] = self.archive_file(self.outer_dir, folder/name,
                                                          'candidate-files/'+model+'/'+name)
            for name, source in ((model+'.manifest.json', folder/'manifest.json'),
                                 (model+'.ota.bin.signed', folder/(ota+'.signed'))):
                deployed[name] = self.archive_file(self.inner_dir, source, 'sources/'+name)
        inner_names = {'controller.py', 'readonly_stress.py', 'deploy_candidate.py',
                       'diagnose_shared_candidate.py', 'ota_package.py', 'status_benchmark.py',
                       'validate_shared_candidate.py'}
        outer_names = inner_names | AUDIT.REQUIRED_ACCEPTANCE_SOURCES | {'initial_controller.py'}
        inner_sources = {name: self.source_record(self.inner_dir, name) for name in inner_names}
        inner_sources.update(deployed)
        outer_sources = {name: self.source_record(self.outer_dir, name) for name in outer_names}
        steps = [dict(self.process(), name='deploy-'+ip.rsplit('.', 1)[1],
                      argv=['python', '/helper/deploy_candidate.py', ip.rsplit('.', 1)[1],
                            self.label+'-initial', '0.9.15-old', self.archive])
                 for ip in AUDIT.TARGETS]
        steps.append(dict(self.process(), name='paced-diagnostic',
                          argv=['python', '/helper/diagnose_shared_candidate.py', '--version',
                                self.version, '--label', self.label, '--execute']))
        self.inner = dict(self.process(), label=self.label, archive=self.archive,
                          firmware_commit=self.commit, version=self.version, previous_version='0.9.15-old',
                          validation_commit=self.commit, completed=True, initial_signed_ota_complete=True,
                          paced_diagnostic_complete=True, source_unchanged=True, source_copies_unchanged=True,
                          source_manifest_unchanged=True, steps=steps, diagnostic_summary=summary,
                          targets={ip: ident for ip, (ident, _) in AUDIT.TARGETS.items()})
        for step in steps:
            step.update(started_at=100, ended_at=110, started_monotonic=1000, ended_monotonic=1010)
        inner_manifest = {'commit': self.commit, 'candidate_commit': self.commit,
                          'candidate_version': self.version, 'sources': inner_sources}
        self.write(self.inner_dir/'source-manifest.json', inner_manifest)
        self.inner['source_manifest_sha256'] = AUDIT._sha256(self.inner_dir/'source-manifest.json')
        self.write(self.inner_dir/'controller.json', self.inner)
        self.initial_row = dict(self.process(), name='initial',
                                evidence_path=str(self.inner_dir/'controller.json'),
                                argv=['python', '/helper/run_initial_shared_screen.py', '--archive', self.archive,
                                      '--commit', self.commit, '--version', self.version,
                                      '--label', self.label, '--execute'])
        self.outer = dict(label=self.label, archive=self.archive, commit=self.commit, version=self.version,
                          status='failed', completed=False, sources=outer_sources, candidate_files=files,
                          validation_commit=self.commit,
                          targets=[ip+'='+ident for ip, (ident, _) in AUDIT.TARGETS.items()],
                          stages=[self.initial_row, {'name': 'pre_stability', 'status': 'failed', 'exit_code': 1}])
        self.write(self.outer_dir/'source-manifest.json',
                   {'validation_commit': self.commit, 'sources': outer_sources})
        self.outer['source_manifest_sha256'] = AUDIT._sha256(self.outer_dir/'source-manifest.json')
        self.write(self.outer_dir/'controller.json', self.outer)
        for ip, (ident, model) in AUDIT.TARGETS.items():
            folder = self.private/('deploy-'+self.label+'-initial-'+ip.rsplit('.', 1)[1])
            state = self.runner()['before'][ip]
            self.write(folder/'before.json', {'status': dict(state['state'], version=self.inner['previous_version'], uptime_seconds=1000),
                                               'config': state['config']})
            self.write(folder/'after.json', {'status': dict(state['state'], uptime_seconds=10), 'config': state['config']})
            package_dir = self.base/'candidates'/self.archive/model
            ota = 'phicomm_'+('tc1_a1' if model == 'tc1' else model)+'.ota.bin'
            self.write(folder/'upload-intent.json', {'ip': ip, 'device_id': ident, 'at': 104.5,
                       'sha256': AUDIT._sha256(package_dir/(ota+'.signed')),
                       'manifest': json.loads((package_dir/'manifest.json').read_text())})
            self.write(folder/'upload.json', {'http': 202})

    @staticmethod
    def digest(data):
        import hashlib
        return hashlib.sha256(data).hexdigest()

    @staticmethod
    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))

    @staticmethod
    def process():
        return {'pid': 123, 'status': 'complete', 'exit_code': 0, 'process_group_empty': True,
                'unexpected_descendants': False, 'descendant_cleanup_attempted': False,
                'timed_out': False, 'failure_evidence': False, 'interrupted': False}

    def archive_file(self, folder, source, destination):
        path = folder/destination
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(source.read_bytes())
        digest = AUDIT._sha256(source)
        return {'source_path': str(source), 'archive_path': destination,
                'sha256': digest, 'archived_sha256': digest}

    def source_record(self, folder, name):
        source = self.base/'current-sources'/name
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text('original '+name)
        return self.archive_file(folder, source, 'sources/'+name)

    def verify(self):
        return AUDIT.verify_reusable_initial(self.label, self.archive, self.commit, self.version)

    def runner(self, shift=0, reset=False):
        before = {}
        for ip, (ident, model) in AUDIT.TARGETS.items():
            state = {'device_id': ident, 'model': model, 'version': self.version,
                     'uptime_seconds': 100+shift if not reset else 2,
                     'requested_fan_on': True, 'requested_speed': 3, 'child_lock': False,
                     'led_enabled': True, 'socket_states': [False]*6, 'brightness_requested': 20}
            before[ip] = {'id': ident, 'state': state, 'config': {'name': model},
                          'status_request_started_monotonic': 1000+shift,
                          'status_request_ended_monotonic': 1000.1+shift,
                          'boot_origin': 900 if not reset else 998+shift}
        return {'version': self.version, 'before': before, 'rows': [], 'memory': [
                    {'ip': ip, 'ok': True, 'request_started_monotonic': 1000+shift,
                     'request_ended_monotonic': 1000.1+shift,
                     'data': {'uptime_ms': (100+shift if not reset else 2)*1000}}
                    for ip in before], 'blockers': [],
                'run': {'status': 'complete', 'completed': True, 'failure_type': None},
                'completion_checks': dict(network_restore=True, configuration_unchanged=True, idle_quiescence=True)}

    def test_correct_reuse_keeps_failed_outer_and_current_sources_may_change(self):
        (self.base/'current-sources/readonly_stress.py').write_text('new retry test code')
        proof = self.verify()
        self.assertEqual(proof['original_outer_status'], 'failed')
        self.assertFalse(proof['original_outer_completed'])
        self.assertTrue(AUDIT.verify_reuse_continuity(proof, self.runner(shift=500))['passed'])
        self.assertEqual(json.loads((self.outer_dir/'controller.json').read_text())['status'], 'failed')

    def test_original_normal_initial_gate_still_passes(self):
        self.assertTrue(AUDIT._initial_gate(self.inner, self.commit, self.version))
        self.inner['targets'] = []
        self.assertFalse(AUDIT._initial_gate(self.inner, self.commit, self.version))

    def mapped_initial(self):
        import copy
        inner = copy.deepcopy(self.inner)
        a1 = tuple(AUDIT.TARGETS)[0]
        inner['previous_versions'] = {a1: '0.9.15-4e5a85'}
        inner['steps'][0]['argv'][4] = inner['previous_versions'][a1]
        folder = self.private/('deploy-'+self.label+'-initial-111')
        before = json.loads((folder/'before.json').read_text())
        before['status']['version'] = inner['previous_versions'][a1]
        self.write(folder/'before.json', before)
        self.outer['stages'][0]['argv'].extend(['--previous-version', inner['previous_version'],
                                                '--previous', a1+'=0.9.15-4e5a85'])
        self.write(self.inner_dir/'controller.json', inner)
        self.write(self.outer_dir/'controller.json', self.outer)
        return inner

    def test_per_device_previous_normal_ota_binds_before_and_candidate_independently(self):
        inner = self.mapped_initial()
        self.assertTrue(AUDIT._initial_gate(inner, self.commit, self.version))
        proof = self.verify()
        self.assertEqual(proof['version'], self.version)
        self.assertEqual(inner['previous_version'], '0.9.15-old')
        resolved = AUDIT._initial_previous_versions(inner)
        self.assertEqual(resolved['10.117.152.111'], '0.9.15-4e5a85')
        self.assertEqual(resolved['10.117.152.102'], inner['previous_version'])
        # Full resolved records are equivalent to a CLI containing only overrides.
        inner['previous_versions'] = resolved
        self.write(self.inner_dir/'controller.json', inner)
        self.verify()

    def test_per_device_previous_invalid_mapping_and_wrong_actual_argv_rejected(self):
        import copy
        valid = self.mapped_initial()
        for mapping in ({}, None, [], {'10.117.152.999':'0.9.15-old'},
                        {'10.117.152.111':''}, {'10.117.152.111':None}, {'10.117.152.111':' bad '},
                        {'10.117.152.111':'not-a-version'}):
            inner = copy.deepcopy(valid);inner['previous_versions'] = mapping
            self.assertFalse(AUDIT._initial_gate(inner, self.commit, self.version))
        bad = copy.deepcopy(valid);bad['steps'][0]['argv'][4] = bad['previous_version']
        self.assertFalse(AUDIT._initial_gate(bad, self.commit, self.version))
        with self.assertRaises(ValueError): AUDIT._initial_previous_versions(dict(valid, previous_version=''))

    def test_per_device_previous_duplicate_unknown_wrong_or_missing_cli_rejected(self):
        import copy
        self.mapped_initial()
        base = copy.deepcopy(self.outer['stages'][0]['argv'])
        variants = [base+['--previous','10.117.152.111=0.9.15-4e5a85'],
                    base+['--previous','10.117.152.999=0.9.15-old'],
                    base+['--previous'], base[:-2],
                    [arg if arg != '10.117.152.111=0.9.15-4e5a85' else '10.117.152.111=0.9.15-wrong' for arg in base]]
        for args in variants:
            self.outer['stages'][0]['argv'] = args
            self.write(self.outer_dir/'controller.json', self.outer)
            with self.assertRaisesRegex(ValueError, 'previous override argv'): self.verify()

    def test_initial_deploy_before_intent_upload_after_are_re_read_and_bound(self):
        self.mapped_initial()
        folder = self.private/('deploy-'+self.label+'-initial-111')
        mutations = [('before', lambda r:r['status'].__setitem__('version', '0.9.15-old')),
                     ('after', lambda r:r['status'].__setitem__('version', '0.9.15-4e5a85')),
                     ('before', lambda r:r['status'].__setitem__('device_id', 'wrong')),
                     ('upload-intent', lambda r:r.__setitem__('sha256', '0'*64)),
                     ('upload-intent', lambda r:r.__setitem__('at', 99999)),
                     ('upload', lambda r:r.__setitem__('http', 400)),
                     ('after', lambda r:r['config'].__setitem__('name','changed')),
                     ('after', lambda r:r['status'].__setitem__('requested_speed', 99))]
        for name, mutate in mutations:
            path = folder/(name+'.json');old = path.read_bytes()
            record = json.loads(old);mutate(record);self.write(path,record)
            try:
                with self.assertRaises(ValueError): self.verify()
            finally:path.write_bytes(old)
        proof = self.verify()
        for name in ('before', 'upload-intent', 'upload', 'after'):
            self.assertIn(str((folder/(name+'.json')).resolve()), proof['proof_sha256'])

    def test_per_device_previous_reused_three_steps_uses_each_actual_target(self):
        from unittest.mock import patch
        inner, proof = self.partial_initial()
        inner['previous_versions'] = {'10.117.152.111': self.version, '10.117.152.102': '0.9.15-other'}
        inner['steps'][0]['argv'][4] = '0.9.15-other'
        with patch.object(AUDIT, 'verify_completed_deploy', return_value=proof):
            self.assertTrue(AUDIT._initial_gate(inner, self.commit, self.version))
            inner['steps'][0]['argv'][4] = inner['previous_version']
            self.assertFalse(AUDIT._initial_gate(inner, self.commit, self.version))
            inner['steps'][0]['argv'][4] = '0.9.15-other'
            inner['previous_versions']['10.117.152.111'] = '0.9.15-wrong'
            self.assertFalse(AUDIT._initial_gate(inner, self.commit, self.version))

    def partial_initial(self):
        import copy
        inner = copy.deepcopy(self.inner)
        inner.update(started_at=100, ended_at=110)
        inner['steps'] = inner['steps'][1:]
        for step in inner['steps']:
            step.update(started_at=104, ended_at=105, started_monotonic=1004, ended_monotonic=1005)
        state = {'device_id': AUDIT.TARGETS['10.117.152.111'][0], 'model': 'a1',
                 'version': self.version, 'uptime_seconds': 5, 'requested_speed': 100}
        proof = {'schema': 'completed-single-deploy-v1', 'source_label': 'failed-source',
                 'ip': '10.117.152.111', 'archive': self.archive, 'commit': self.commit,
                 'version': self.version, 'previous_version': inner['previous_version'],
                 'after': {'status': state, 'config': {'name': 'unchanged'}},
                 'original_step': {'pid': 999, 'ended_at': 99}, 'proof_sha256': {}}
        samples = []
        for index in range(3):
            current = dict(state, uptime_seconds=8+index)
            samples.append({'status': current, 'config': {'name': 'unchanged'},
                            'started_at': 102+index, 'ended_at': 102.1+index,
                            'started_monotonic': 1002+index, 'ended_monotonic': 1002.1+index})
        fresh = AUDIT.verify_completed_deploy_fresh(proof, samples)
        inner['reused_deployments'] = [dict(proof, fresh_continuity=fresh)]
        return inner, proof

    def test_partial_initial_uses_two_real_ota_children_and_explicit_history(self):
        from unittest.mock import patch
        inner, proof = self.partial_initial()
        with patch.object(AUDIT, 'verify_completed_deploy', return_value=proof):
            self.assertTrue(AUDIT._initial_gate(inner, self.commit, self.version))
            self.write(self.inner_dir/'controller.json', inner)
            self.outer['stages'][0]['argv'].extend(['--reuse-completed-deploy', 'failed-source'])
            self.write(self.outer_dir/'controller.json', self.outer)
            reused = self.verify()
            self.assertEqual(reused['original_outer_status'], 'failed')
            self.assertEqual(reused['original_initial_stage']['pid'], inner['pid'])
            self.assertEqual(len(inner['steps']), 3)
            self.assertEqual(inner['reused_deployments'][0]['original_step']['pid'], 999)
            self.assertNotIn('started_monotonic', inner['reused_deployments'][0]['original_step'])

    def accepted_partial_initial(self):
        from unittest.mock import patch
        fixture_version = patch.object(AUDIT.ACCEPTED_UPLOAD, 'VERSION', self.version)
        fixture_version.start()
        self.addCleanup(fixture_version.stop)
        inner, proof = self.partial_initial()
        proof.update(schema=AUDIT.ACCEPTED_UPLOAD.SCHEMA, cleanup_file='/fixture/cleanup-proof.json',
                     validation_commit=self.commit, postcheck_step={'started_at': 98, 'ended_at': 99},
                     original_failed_step={'exit_code': 1, 'status': 'failed'})
        proof['original_step'] = dict(proof['postcheck_step'], name='independent-upload-postcheck')
        runtime = dict(requested_fan_on=False, child_lock=False, led_enabled=True,
                       firmware_variant='full', full_resources=True, bootstrap_ui=False,
                       wifi_connected=True, mqtt_connected=True, ota_active=False,
                       reboot_pending=False, ota_state='idle', ota_error='')
        proof['after']['status'].update(runtime)
        fresh = inner['reused_deployments'][0]['fresh_continuity']
        for sample in fresh['samples']:
            sample['status'].update(runtime)
        fresh['schema'] = 'accepted-upload-fresh-v1'
        inner['reused_deployments'] = [dict(proof, fresh_continuity=fresh)]
        return inner, proof

    def archive_accepted_source(self, inner):
        for folder, record in ((self.inner_dir, inner), (self.outer_dir, self.outer)):
            path = folder/'source-manifest.json'
            manifest = json.loads(path.read_text())
            manifest['sources']['accepted_upload_postcheck.py'] = self.source_record(folder, 'accepted_upload_postcheck.py')
            self.write(path, manifest)
            record['source_manifest_sha256'] = AUDIT._sha256(path)
            if folder == self.outer_dir:
                record['sources'] = manifest['sources']
        self.write(self.inner_dir/'controller.json', inner)
        self.write(self.outer_dir/'controller.json', self.outer)

    def test_accepted_schema_dispatch_and_exact_cli_preserve_failed_history(self):
        from unittest.mock import patch
        inner, proof = self.accepted_partial_initial()
        with patch.object(AUDIT.ACCEPTED_UPLOAD, 'verify_accepted_upload_postcheck', return_value=proof), \
             patch.object(AUDIT, 'verify_completed_deploy', side_effect=AssertionError('old path used')):
            self.assertTrue(AUDIT._initial_gate(inner, self.commit, self.version))
            self.outer['stages'][0]['argv'].extend(['--reuse-accepted-upload-postcheck', 'failed-source'])
            self.archive_accepted_source(inner)
            result = self.verify()
            self.assertEqual(result['original_outer_status'], 'failed')
            self.assertEqual(inner['reused_deployments'][0]['original_failed_step']['exit_code'], 1)
            self.assertEqual(len(inner['steps']), 3)
            for suffix in (['--reuse-completed-deploy', 'failed-source'],
                           ['--reuse-accepted-upload-postcheck', 'wrong-source'],
                           ['--reuse-accepted-upload-postcheck', 'failed-source', '--reuse-completed-deploy', 'failed-source'],
                           ['--reuse-accepted-upload-postcheck', 'failed-source']*2):
                self.outer['stages'][0]['argv'] = self.outer['stages'][0]['argv'][:-2]+suffix
                self.write(self.outer_dir/'controller.json', self.outer)
                with self.assertRaises(ValueError): self.verify()
                self.outer['stages'][0]['argv'] = self.outer['stages'][0]['argv'][:-len(suffix)]+['--reuse-accepted-upload-postcheck', 'failed-source']

    def test_accepted_schema_unknown_wrong_candidate_outside_window_rejected(self):
        import copy
        from unittest.mock import patch
        original, proof = self.accepted_partial_initial()
        mutations = [lambda i: i['reused_deployments'][0].__setitem__('schema', 'unknown'),
                     lambda i: i['reused_deployments'][0].__setitem__('commit', '0'*40),
                     lambda i: i['reused_deployments'][0]['fresh_continuity']['samples'][0].__setitem__('started_at', 97),
                     lambda i: i.__setitem__('started_at', 103),
                     lambda i: i['reused_deployments'][0]['fresh_continuity']['samples'][0]['status'].__setitem__('mqtt_connected', False)]
        for mutate in mutations:
            inner = copy.deepcopy(original);mutate(inner)
            with patch.object(AUDIT.ACCEPTED_UPLOAD, 'verify_accepted_upload_postcheck', return_value=proof), \
                 patch.object(AUDIT, 'verify_completed_deploy', side_effect=AssertionError('old fallback used')):
                self.assertFalse(AUDIT._initial_gate(inner, self.commit, self.version))

    def test_accepted_verifier_source_archive_required_and_hash_checked(self):
        from unittest.mock import patch
        inner, proof = self.accepted_partial_initial()
        self.outer['stages'][0]['argv'].extend(['--reuse-accepted-upload-postcheck', 'failed-source'])
        self.write(self.inner_dir/'controller.json', inner)
        self.write(self.outer_dir/'controller.json', self.outer)
        with patch.object(AUDIT.ACCEPTED_UPLOAD, 'verify_accepted_upload_postcheck', return_value=proof):
            with self.assertRaisesRegex(ValueError, 'verifier source archive missing'):self.verify()
            self.archive_accepted_source(inner)
            self.verify()
            (self.inner_dir/'sources/accepted_upload_postcheck.py').write_text('tampered')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):self.verify()

    def test_partial_initial_rejects_missing_duplicate_or_replayed_evidence(self):
        import copy
        from unittest.mock import patch
        original, proof = self.partial_initial()
        for case in ('no-history', 'duplicate-history', 'a1-child', 'missing-child',
                     'wrong-order', 'stale-read', 'changed-config', 'no-monotonic', 'changed-provenance'):
            with self.subTest(case=case), patch.object(AUDIT, 'verify_completed_deploy', return_value=proof):
                inner = copy.deepcopy(original)
                if case == 'no-history':
                    inner.pop('reused_deployments')
                elif case == 'duplicate-history':
                    inner['reused_deployments'] *= 2
                elif case == 'a1-child':
                    inner['steps'][0]['argv'][2] = '111'
                elif case == 'missing-child':
                    inner['steps'].pop(0)
                elif case == 'wrong-order':
                    inner['steps'][0], inner['steps'][1] = inner['steps'][1], inner['steps'][0]
                elif case == 'stale-read':
                    inner['started_at'] = 103
                elif case == 'changed-config':
                    inner['reused_deployments'][0]['fresh_continuity']['samples'][0]['config'] = {}
                elif case == 'no-monotonic':
                    inner['steps'][0].pop('started_monotonic')
                else:
                    inner['reused_deployments'][0]['original_step']['pid'] = 1000
                self.assertFalse(AUDIT._initial_gate(inner, self.commit, self.version))

    def test_partial_fresh_reboot_boundary_and_sdk_fast_clock(self):
        import copy
        inner, proof = self.partial_initial()
        samples = inner['reused_deployments'][0]['fresh_continuity']['samples']
        for index, sample in enumerate(samples):
            sample['status']['uptime_seconds'] += 100+index
        self.assertTrue(AUDIT.verify_completed_deploy_fresh(proof, samples)['passed'])
        reboot = copy.deepcopy(samples)
        reboot[0]['status']['uptime_seconds'] = 5
        reboot[0]['started_at'] = 109
        reboot[0]['ended_at'] = 109.1
        with self.assertRaises(ValueError):
            AUDIT.verify_completed_deploy_fresh(proof, reboot)

    def test_missing_and_changed_archive_fail(self):
        path = self.outer_dir/'sources/controller.py'
        path.unlink()
        with self.assertRaises((ValueError, FileNotFoundError)):
            self.verify()
        path.write_text('forged source')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            self.verify()

    def test_failed_inner_or_step_or_outer_initial_never_reused(self):
        import copy
        for case in ('inner', 'step', 'outer'):
            with self.subTest(case=case):
                inner, outer = copy.deepcopy(self.inner), copy.deepcopy(self.outer)
                if case == 'inner':
                    inner['completed'] = False
                elif case == 'step':
                    inner['steps'][1]['exit_code'] = 1
                else:
                    outer['stages'][0]['process_group_empty'] = False
                self.write(self.inner_dir/'controller.json', inner)
                self.write(self.outer_dir/'controller.json', outer)
                with self.assertRaises(ValueError):
                    self.verify()

    def test_wrong_candidate_or_targets_or_step_order_fail(self):
        with self.assertRaises(ValueError):
            AUDIT.verify_reusable_initial(self.label, self.archive, 'b'*40, self.version)
        self.inner['steps'][0], self.inner['steps'][1] = self.inner['steps'][1], self.inner['steps'][0]
        self.write(self.inner_dir/'controller.json', self.inner)
        with self.assertRaisesRegex(ValueError, 'argv/order|inner initial did not pass'):
            self.verify()

    def test_signed_package_changed_or_manifest_changed_fail(self):
        record = self.outer['candidate_files']['a1/phicomm_a1.ota.bin.signed']
        from pathlib import Path
        Path(record['source_path']).write_bytes(b'altered')
        with self.assertRaisesRegex(ValueError, 'candidate changed'):
            self.verify()

    def test_paced_truncation_and_summary_tamper_fail(self):
        self.paced['rows'].pop()
        self.write(self.paced_path, self.paced)
        with self.assertRaisesRegex(ValueError, 'request count'):
            self.verify()
        self.paced['rows'].append({'ip': list(AUDIT.TARGETS)[-1], 'phase': 'paced-live', 'http': 503})
        self.write(self.paced_path, self.paced)
        with self.assertRaisesRegex(ValueError, 'summary differs'):
            self.verify()

    def test_reset_config_control_and_identity_changes_fail_before_load(self):
        proof = self.verify()
        for case in ('reset', 'config', 'control', 'identity'):
            with self.subTest(case=case):
                pre = self.runner(shift=500, reset=(case == 'reset'))
                ip = next(iter(AUDIT.TARGETS))
                if case == 'config':
                    pre['before'][ip]['config']['name'] = 'changed'
                elif case == 'control':
                    pre['before'][ip]['state']['requested_speed'] = 4
                elif case == 'identity':
                    pre['before'][ip]['state']['device_id'] = 'other'
                with self.assertRaises(ValueError):
                    AUDIT.verify_reuse_continuity(proof, pre)

    def test_original_proof_change_detected_after_reuse(self):
        proof = self.verify()
        self.paced['before'][next(iter(AUDIT.TARGETS))]['config']['name'] = 'changed'
        self.write(self.paced_path, self.paced)
        with self.assertRaisesRegex(ValueError, 'paced evidence changed'):
            AUDIT.verify_reuse_continuity(proof, self.runner(shift=500))


    def checkpoint_and_uploads(self):
        import copy
        label = 'new-r3'
        proof = self.verify()
        checkpoint_runner = self.runner(shift=500)
        checkpoint = {'schema': 'reuse-pre-checkpoint-v1', 'reuse_record_sha256': 'record-hash',
                      'captured_monotonic': 1501, 'runner': checkpoint_runner,
                      'continuity': AUDIT.verify_reuse_continuity(proof, checkpoint_runner,
                                                                 require_complete=False)}
        checkpoint_path = self.private/'checkpoint.json'
        journal_path = self.private/'journal.json'
        pre = self.runner(shift=1000)
        steps = []
        for index, (ip, (_, model)) in enumerate(AUDIT.TARGETS.items()):
            suffix = ip.rsplit('.', 1)[1]
            start, end = 2000+100*index, 2020+100*index
            steps.append({'ip': ip, 'pid': 10+index, 'exit_code': 0,
                          'started_monotonic': start, 'ended_monotonic': end,
                          'argv': ['python', '/helper/deploy_candidate.py', suffix,
                                   label+'-pre-ota-out', self.version, self.archive]})
            folder = self.private/('deploy-'+label+'-pre-ota-out-'+suffix)
            base = checkpoint_runner['before'][ip]
            before = {'status': copy.deepcopy(base['state']), 'config': base['config']}
            after = copy.deepcopy(before)
            after['status']['uptime_seconds'] = 5
            self.write(folder/'before.json', before)
            self.write(folder/'after.json', after)
            self.write(folder/'upload.json', {'http': 202})
            manifest = json.loads((self.base/'candidates'/self.archive/model/'manifest.json').read_text())
            self.write(folder/'upload-intent.json', {'ip': ip, 'device_id': base['id'],
                       'sha256': proof['signed_package_sha256'][model], 'manifest': manifest})
            pre['before'][ip].update(status_request_started_monotonic=end+60,
                                     status_request_ended_monotonic=end+60.1, boot_origin=end-10)
            pre['before'][ip]['state']['uptime_seconds'] = 70
            pre['memory'][index].update(request_started_monotonic=end+60,
                                       request_ended_monotonic=end+60.1, data={'uptime_ms': 70000})
        journal = {'schema': 'reuse-ota-out-v1', 'stage': label+'-pre-ota-out',
                   'completed': True, 'steps': steps}
        self.write(checkpoint_path, checkpoint)
        self.write(journal_path, journal)
        return proof, checkpoint_path, journal_path, pre, label

    def test_checkpoint_precedes_three_signed_uploads_and_postboot_is_new_anchor(self):
        args = self.checkpoint_and_uploads()
        result = AUDIT.verify_reused_pre(*args, 'record-hash')
        self.assertTrue(result['passed'])
        self.assertEqual(result['ota_out_count'], 3)
        self.assertEqual(len(result['proof_sha256']), 14)

    def test_missing_altered_or_late_checkpoint_and_wrong_package_fail(self):
        import copy
        args = self.checkpoint_and_uploads()
        proof, path, journal_path, pre, label = args
        original = json.loads(path.read_text())
        for case in ('missing', 'config', 'reset', 'late', 'hash'):
            with self.subTest(case=case):
                checkpoint = copy.deepcopy(original)
                if case == 'missing':
                    path.unlink()
                else:
                    if case == 'config':
                        checkpoint['runner']['before'][next(iter(AUDIT.TARGETS))]['config']['name'] = 'bad'
                    elif case == 'reset':
                        checkpoint['runner'] = self.runner(shift=500, reset=True)
                    elif case == 'late':
                        checkpoint['captured_monotonic'] = 2500
                    else:
                        checkpoint['reuse_record_sha256'] = 'bad'
                    self.write(path, checkpoint)
                with self.assertRaises((ValueError, FileNotFoundError)):
                    AUDIT.verify_reused_pre(*args, 'record-hash')
                self.write(path, original)
        folder = self.private/('deploy-'+label+'-pre-ota-out-111')
        intent = json.loads((folder/'upload-intent.json').read_text())
        intent['sha256'] = 'bad'
        self.write(folder/'upload-intent.json', intent)
        with self.assertRaisesRegex(ValueError, 'package/target'):
            AUDIT.verify_reused_pre(*args, 'record-hash')

    def test_infinite_uptime_and_incomplete_or_reordered_uploads_fail(self):
        import copy
        args = self.checkpoint_and_uploads()
        proof, checkpoint, journal_path, pre, label = args
        original = json.loads(journal_path.read_text())
        for case in ('incomplete', 'reverse', 'exit', 'before-checkpoint'):
            journal = copy.deepcopy(original)
            if case == 'incomplete':
                journal['steps'].pop()
            elif case == 'reverse':
                journal['steps'].reverse()
            elif case == 'exit':
                journal['steps'][0]['exit_code'] = 1
            else:
                journal['steps'][0]['started_monotonic'] = 1400
            self.write(journal_path, journal)
            with self.subTest(case=case), self.assertRaises(ValueError):
                AUDIT.verify_reused_pre(*args, 'record-hash')
        self.write(journal_path, original)
        path = self.private/('deploy-'+label+'-pre-ota-out-111')/'before.json'
        before = json.loads(path.read_text())
        before['status']['uptime_seconds'] = float('inf')
        self.write(path, before)
        with self.assertRaisesRegex(ValueError, 'planned reboot'):
            AUDIT.verify_reused_pre(*args, 'record-hash')

    def test_real_memory_stream_is_required_and_millisecond_reset_is_rejected(self):
        import copy
        old, new = self.runner(), self.runner(shift=500)
        ip = next(iter(AUDIT.TARGETS))
        for case in ('missing', 'missing-counter', 'millisecond-reset'):
            changed = copy.deepcopy(new)
            if case == 'missing':
                changed['memory'] = [row for row in changed['memory'] if row['ip'] != ip]
            elif case == 'missing-counter':
                changed['memory'][0]['data'] = {}
            else:
                # Status remains continuous; memory differs by 0.5s. Giving
                # memory a false 1s quantum would hide this forward origin shift.
                changed['memory'][0]['data']['uptime_ms'] -= 500
            with self.subTest(case=case), self.assertRaises(ValueError):
                AUDIT.verify_runner_continuity(old, changed, self.version)

    def test_cross_stage_chain_does_not_rebase_away_reset_or_control_change(self):
        import copy
        pre, middle, final = self.runner(), self.runner(shift=200), self.runner(shift=400)
        report = AUDIT.verify_post_ota_chain(pre, [('http', middle), ('soak', final)], self.version)
        self.assertTrue(report['passed'])
        for case in ('reset', 'config', 'control'):
            changed = copy.deepcopy(final)
            if case == 'reset':
                changed = self.runner(shift=400, reset=True)
            elif case == 'config':
                changed['before'][next(iter(AUDIT.TARGETS))]['config']['name'] = 'changed'
            else:
                changed['before'][next(iter(AUDIT.TARGETS))]['state']['requested_speed'] += 1
            with self.subTest(case=case), self.assertRaises(ValueError):
                AUDIT.verify_post_ota_chain(pre, [('http', middle), ('soak', changed)], self.version)

    def test_postcheck_uses_recorded_fresh_intervals_instead_of_old_soak_before(self):
        import copy
        soak, fresh = self.runner(shift=400), self.runner(shift=500)
        comparison = {}
        for ip in AUDIT.TARGETS:
            baseline = fresh['before'][ip]
            comparison[ip] = dict(identity_unchanged=True, model_version_unchanged=True,
                                  controls_unchanged=True, config_unchanged=True, uptime_continuity=True,
                                  fresh_status_interval=[baseline['status_request_started_monotonic'],
                                                         baseline['status_request_ended_monotonic']],
                                  fresh_uptime_seconds=baseline['state']['uptime_seconds'])
        fresh['before'] = copy.deepcopy(soak['before'])
        post = {'fresh_baseline_comparison': comparison}
        self.assertTrue(AUDIT.verify_post_ota_chain(soak, [], self.version, post, fresh)['passed'])
        comparison[next(iter(AUDIT.TARGETS))]['fresh_uptime_seconds'] = 2
        with self.assertRaises(ValueError):
            AUDIT.verify_post_ota_chain(soak, [], self.version, post, fresh)


    def test_pre_soak_partial_gate_hashes_real_baselines_and_rejects_interstage_reset(self):
        paths = {}
        for name, shift in (('pre_stability', 0), ('http_schema_samples', 100),
                            ('http_regression', 200)):
            folder = self.private/name
            path = folder/('http-bodies.json' if name == 'http_schema_samples' else 'evidence.json')
            self.write(folder/'evidence.json', self.runner(shift=shift))
            if name == 'http_schema_samples':
                self.write(path, {})
            paths[name] = {'evidence_path': str(path)}
        short_output = self.private/'short-output'
        short_runner_path = short_output/'http-runner/evidence.json'
        self.write(short_runner_path, self.runner(shift=300))
        short_controller_path = self.private/'short-controller/controller.json'
        self.write(short_controller_path, {'output': str(short_output)})
        paths['mixed_short'] = {'evidence_path': str(short_controller_path)}
        result = AUDIT.verify_pre_soak_continuity(paths, self.version)
        self.assertTrue(result['passed'])
        self.assertEqual(len(result['proof_sha256']), 5)
        self.write(short_runner_path, self.runner(shift=300, reset=True))
        with self.assertRaisesRegex(ValueError, 'boot changed'):
            AUDIT.verify_pre_soak_continuity(paths, self.version)


    def test_same_device_faster_uptime_clock_is_not_misclassified_as_reset(self):
        old, new = self.runner(), self.runner(shift=500)
        for baseline in new['before'].values():
            baseline['state']['uptime_seconds'] += 3
        for row in new['memory']:
            row['data']['uptime_ms'] += 5000
        self.assertTrue(AUDIT.verify_runner_continuity(old, new, self.version)['passed'])


if __name__ == '__main__':
    unittest.main()
