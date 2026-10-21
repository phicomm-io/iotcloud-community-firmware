"""Offline schema and fail-closed regressions for the shared-candidate audit."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics
import tempfile
import unittest


from tests.device import audit_shared_candidate_completion as AUDIT


VERSION = '0.9.15-testcandidate'
LABEL = 'test-candidate'
SOURCE = b'offline mock runner source\n'


def make_fixture():
    targets = []
    before = {}
    for ip, (ident, model) in AUDIT.TARGETS.items():
        targets.append({'ip': ip, 'device_id': ident, 'version': VERSION})
        before[ip] = {
            'id': ident,
            'config': {'credential': 'fixture-secret-must-not-escape'},
            'state': {'device_id': ident, 'model': model, 'version': VERSION,
                      'uptime_seconds': 10, 'wifi_connected': True,
                      'mqtt_connected': True},
            'status_request_started_monotonic': 899.5,
            'status_request_ended_monotonic': 900.0,
        }
    phases_plan = [
        {'name': name, 'clients_per_device': clients, 'requested_seconds': seconds}
        for name, (clients, seconds) in AUDIT.PHASES.items()
    ]
    plan_data = {
        'mode': 'full', 'targets': targets, 'steps': list(AUDIT.FULL_STEPS),
        'phases': phases_plan, 'soak_seconds': 14400, 'soak_clients': 4,
        'soak_period': 1,
    }
    argv = ['python3', 'tests/device/readonly_stress.py', '--version', VERSION,
            '--normal-phase-seconds', '600', '--phase-seconds', '120',
            '--soak-seconds', '14400', '--soak-clients', '4', '--soak-period', '1',
            '--require-shared-core']
    for ip, (ident, _) in AUDIT.TARGETS.items():
        argv.extend(['--target', ip+'='+ident])
    pid = 4567
    external_plan = {
        'process': {'pid': pid, 'argv': argv},
        'phase_plan_seconds': {name: seconds for name, (_, seconds) in AUDIT.PHASES.items()},
        'runner_sha256': hashlib.sha256(SOURCE).hexdigest(),
    }
    evidence = {
        'version': VERSION, 'target_versions': {}, 'blockers': [],
        'before': before,
        'run': {'run_id': 'offline-run', 'status': 'complete', 'completed': True,
                'failure_type': None, 'plan_completion_required': True,
                'require_shared_core': True, 'started_at': 100.0,
                'ended_at': 15400.0, 'plan': plan_data},
        'completion_checks': {'network_restore': True, 'configuration_unchanged': True,
                              'idle_quiescence': True},
        'phases': [], 'scenarios': [], 'recoveries': [], 'rows': [], 'memory': [],
    }
    for index, (name, (clients, seconds)) in enumerate(AUDIT.PHASES.items()):
        evidence['phases'].append({
            'name': name, 'clients_per_device': clients, 'requested_seconds': seconds,
            'elapsed_seconds': seconds + .2, 'completed': True,
        })
        evidence['scenarios'].append({'name': name, 'completed': True})
    for name in AUDIT.FULL_STEPS:
        if name in ('concurrency-1', 'concurrency-4', 'concurrency-8', 'concurrency-16', 'soak'):
            continue
        evidence['scenarios'].append({'name': name, 'completed': True})
    recovery_names = [name+'-recovery' for name in AUDIT.PHASES] + [
        'incomplete-headers-recovery', 'aborted-connections-recovery',
        'buffered-read-input-recovery', 'missing-read-eof-recovery',
    ]
    for name in recovery_names:
        evidence['recoveries'].append({
            'name': name, 'completed': True,
            'targets': {ip: {'completed': True, 'consecutive_successes': 3}
                        for ip in AUDIT.TARGETS},
        })
    for ip in AUDIT.TARGETS:
        for ix, timestamp in enumerate((200.0, 300.0)):
            evidence['rows'].append({
                'ip': ip, 'phase': 'soak', 'action': 'status.get', 'params': {'view': 'live'},
                'at': timestamp+ix, 'http': 200, 'code': 'ok', 'ok': True,
                'uptime': 110+100*ix, 'ms': 20,
            })
        model = AUDIT.TARGETS[ip][1]
        cap = AUDIT.MODEL_CAP[model]
        def stats(free, uptime_ms):
            return {
                'free_memory': free, 'json_live_blocks': 0, 'memory_active': 0,
                'memory_reserved': 0, 'memory_waiting': 0, 'http_core_occupied': 1,
                'http_core_owner_state': 1, 'http_core_close_failures': 0,
                'http_core_restarts': 0, 'http_core_tcp_limit': cap,
                'http_core_tcp_rc': 0, 'http_core_server_bytes': 64,
                'http_tcp_sample_ms': 1, 'uptime_ms': uptime_ms,
            }
        for index in range(8):
            start = 1000.0 + index * 2
            evidence['memory'].append({'ip': ip, 'phase': 'baseline', 'http': 200,
                                       'ok': True, 'request_started_monotonic': start,
                                       'request_ended_monotonic': start + .1,
                                       'data': stats(1000, 100000 + index * 2000)})
            evidence['memory'].append({'ip': ip, 'phase': 'final', 'http': 200,
                                       'ok': True, 'request_started_monotonic': 2000 + index * 2,
                                       'request_ended_monotonic': 2000 + index * 2 + .1,
                                       'data': stats(500, 200000 + index * 2000)})
    process_exit = {'pid': pid, 'exit_code': 0, 'runner_source_unchanged': True}
    return evidence, external_plan, process_exit


def run_audit(evidence=None, plan=None, process_exit=None, source=SOURCE):
    ev, pl, ex = make_fixture()
    return AUDIT._audit(evidence if evidence is not None else ev,
                        plan if plan is not None else pl,
                        process_exit if process_exit is not None else ex,
                        source, LABEL, VERSION)


class AuditTests(unittest.TestCase):
    def test_complete_candidate_passes_without_release_approval_or_secrets(self):
        evidence, plan, process_exit = make_fixture()
        result = run_audit(evidence, plan, process_exit)
        self.assertTrue(result['completion_audit_pass'], result['reasons'])
        memory = result['memory_by_target']['10.117.152.111']
        self.assertEqual(memory['baseline_steady_samples'], 8)
        self.assertEqual(memory['final_steady_samples'], 8)
        self.assertEqual(memory['median_free_memory_loss_bytes'], 500)
        self.assertTrue(memory['memory_uptime']['ok'])
        self.assertEqual(result['release_decision'], 'not_assessed')
        self.assertFalse(result['memory_by_target']['10.117.152.111']['leak_free_proven'])
        serialized = json.dumps(result)
        self.assertNotIn('fixture-secret', serialized)

    def test_missing_or_short_phase_fails_closed(self):
        evidence, plan, process_exit = make_fixture()
        evidence['phases'] = [p for p in evidence['phases'] if p['name'] != 'concurrency-4']
        result = run_audit(evidence, plan, process_exit)
        self.assertFalse(result['completion_audit_pass'])
        self.assertIn('phase_incomplete_or_wrong_duration:concurrency-4', result['reasons'])
        evidence, plan, process_exit = make_fixture()
        evidence['phases'][0]['elapsed_seconds'] = 599
        result = run_audit(evidence, plan, process_exit)
        self.assertFalse(result['completion_audit_pass'])

    def test_partial_run_and_nonzero_or_unobserved_exit_fail(self):
        evidence, plan, process_exit = make_fixture()
        evidence['run']['completed'] = False
        self.assertFalse(run_audit(evidence, plan, process_exit)['completion_audit_pass'])
        evidence, plan, process_exit = make_fixture()
        process_exit['exit_code'] = 3
        self.assertIn('process_exit_not_zero_or_pid_mismatch',
                      run_audit(evidence, plan, process_exit)['reasons'])
        process_exit['exit_code'] = None
        self.assertFalse(run_audit(evidence, plan, process_exit)['completion_audit_pass'])
        evidence, plan, process_exit = make_fixture()
        process_exit['runner_source_unchanged'] = False
        self.assertIn('runner_source_changed_during_run',
                      run_audit(evidence, plan, process_exit)['reasons'])

    def test_exact_script_and_module_entrypoints_are_supported(self):
        evidence, plan, process_exit = make_fixture()
        plan['process']['argv'][1:2] = ['-m']
        plan['process']['argv'][2:2] = ['tests.device.readonly_stress']
        result = run_audit(evidence, plan, process_exit)
        self.assertTrue(result['completion_audit_pass'], result['reasons'])
        evidence, plan, process_exit = make_fixture()
        plan['process']['argv'][1:3] = ['-m', 'tests.device.unrelated_module']
        self.assertIn('process_argv_missing_or_unexpected',
                      run_audit(evidence, plan, process_exit)['reasons'])

    def test_identity_version_uptime_and_http_regressions_are_rejected(self):
        evidence, plan, process_exit = make_fixture()
        evidence['run']['plan']['targets'][0]['device_id'] = 'wrong'
        self.assertFalse(run_audit(evidence, plan, process_exit)['completion_audit_pass'])
        evidence, plan, process_exit = make_fixture()
        evidence['rows'][-1]['uptime'] = 1
        self.assertIn('uptime_missing_or_decreased:10.117.152.110',
                      run_audit(evidence, plan, process_exit)['reasons'])
        evidence, plan, process_exit = make_fixture()
        ip = '10.117.152.111'
        evidence['rows'].extend([
            {'ip': ip, 'phase': 'soak', 'action': 'status.get', 'at': 395.0,
             'ms': 10000, 'http': 200, 'code': 'ok', 'ok': True, 'uptime': 315},
            {'ip': ip, 'phase': 'soak', 'action': 'status.get', 'at': 400.0,
             'ms': 100, 'http': 200, 'code': 'ok', 'ok': True, 'uptime': 310},
        ])
        self.assertTrue(run_audit(evidence, plan, process_exit)['completion_audit_pass'])
        evidence, plan, process_exit = make_fixture()
        evidence['rows'][0]['http'] = 500
        self.assertIn('illegal_http_status_rows', run_audit(evidence, plan, process_exit)['reasons'])
        evidence, plan, process_exit = make_fixture()
        evidence['rows'][0]['code'] = 'internal_error'
        self.assertIn('unexpected_http_200_result', run_audit(evidence, plan, process_exit)['reasons'])

    def test_owner_restart_and_missing_idle_release_fail(self):
        evidence, plan, process_exit = make_fixture()
        evidence['memory'][0]['data']['http_core_restarts'] = 1
        self.assertIn('memory_or_shared_owner_recovery_invalid:10.117.152.111',
                      run_audit(evidence, plan, process_exit)['reasons'])
        evidence, plan, process_exit = make_fixture()
        evidence['memory'][-1]['data']['json_live_blocks'] = 1
        self.assertFalse(run_audit(evidence, plan, process_exit)['completion_audit_pass'])
        evidence, plan, process_exit = make_fixture()
        evidence['memory'][-1]['data']['http_core_occupied'] = 2
        self.assertFalse(run_audit(evidence, plan, process_exit)['completion_audit_pass'])

    def test_busy_baseline_samples_cannot_mask_steady_heap_loss(self):
        evidence, plan, process_exit = make_fixture()
        ip = '10.117.152.111'
        evidence['memory'] = [row for row in evidence['memory']
                              if row['ip'] != ip or row['phase'] not in ('baseline', 'final')]
        cap = AUDIT.MODEL_CAP['a1']

        def data(free, uptime_ms, busy=False):
            return {
                'free_memory': free, 'json_live_blocks': 27 if busy else 0,
                'memory_active': 1 if busy else 0,
                'memory_reserved': 512 if busy else 0, 'memory_waiting': 0,
                'http_core_occupied': 1, 'http_core_owner_state': 1,
                'http_core_close_failures': 0, 'http_core_restarts': 0,
                'http_core_tcp_limit': cap, 'http_core_tcp_rc': 0,
                'http_core_server_bytes': 64, 'http_tcp_sample_ms': 1,
                'uptime_ms': uptime_ms,
            }

        for index in range(7):
            start = 3000 + index * 2
            evidence['memory'].append({
                'ip': ip, 'phase': 'baseline', 'ok': True, 'http': 200,
                'request_started_monotonic': start,
                'request_ended_monotonic': start + .1,
                'data': data(25000, 300000 + index * 1000, busy=True),
            })
        for index in range(3):
            start = 3020 + index * 2
            evidence['memory'].append({
                'ip': ip, 'phase': 'baseline', 'ok': True, 'http': 200,
                'request_started_monotonic': start,
                'request_ended_monotonic': start + .1,
                'data': data(30000, 310000 + index * 1000),
            })
        for index in range(10):
            start = 4000 + index * 2
            evidence['memory'].append({
                'ip': ip, 'phase': 'final', 'ok': True, 'http': 200,
                'request_started_monotonic': start,
                'request_ended_monotonic': start + .1,
                'data': data(28000, 400000 + index * 1000),
            })
        raw_baseline_median = statistics.median(
            row['data']['free_memory'] for row in evidence['memory']
            if row['ip'] == ip and row['phase'] == 'baseline')
        raw_final_median = statistics.median(
            row['data']['free_memory'] for row in evidence['memory']
            if row['ip'] == ip and row['phase'] == 'final')
        self.assertLessEqual(raw_baseline_median - raw_final_median, 1024)
        result = run_audit(evidence, plan, process_exit)
        self.assertFalse(result['completion_audit_pass'])
        self.assertIn('coarse_heap_threshold_failed:'+ip, result['reasons'])
        audited = result['memory_by_target'][ip]
        self.assertEqual(audited['baseline_samples'], 10)
        self.assertEqual(audited['baseline_steady_samples'], 3)
        self.assertEqual(audited['baseline_resource_active_samples'], 7)
        self.assertEqual(audited['baseline_nonsteady_samples'], 7)
        self.assertEqual(audited['final_steady_samples'], 10)
        self.assertEqual(audited['median_free_memory_loss_bytes'], 2000)

    def test_memory_uptime_uses_nonoverlap_and_detects_rollback(self):
        evidence, plan, process_exit = make_fixture()
        ip = '10.117.152.111'
        rows = [row for row in evidence['memory'] if row['ip'] == ip]
        baseline = [row for row in rows if row['phase'] == 'baseline']
        # A lower value in an overlapping sample is not ordered against the
        # previous response; the next non-overlapping rollback is a blocker.
        baseline[1]['request_started_monotonic'] = baseline[0]['request_started_monotonic'] + .05
        baseline[1]['request_ended_monotonic'] = baseline[0]['request_ended_monotonic'] + .1
        baseline[1]['data']['uptime_ms'] = baseline[0]['data']['uptime_ms'] - 1
        self.assertTrue(run_audit(evidence, plan, process_exit)['completion_audit_pass'])
        baseline[2]['data']['uptime_ms'] = baseline[1]['data']['uptime_ms'] - 1
        result = run_audit(evidence, plan, process_exit)
        self.assertFalse(result['completion_audit_pass'])
        self.assertIn('memory_uptime_missing_or_decreased:'+ip, result['reasons'])

    def test_successful_memory_sample_missing_monotonic_metadata_fails(self):
        evidence, plan, process_exit = make_fixture()
        row = next(row for row in evidence['memory'] if row['ip'] == '10.117.152.102')
        del row['request_started_monotonic']
        result = run_audit(evidence, plan, process_exit)
        self.assertIn('memory_uptime_missing_or_decreased:10.117.152.102', result['reasons'])

    def test_resource_zero_without_owner_fields_is_unverified_not_unhealthy(self):
        evidence, plan, process_exit = make_fixture()
        ip = '10.117.152.111'
        row = next(row for row in evidence['memory']
                   if row['ip'] == ip and row['phase'] == 'baseline')
        row['data'].pop('http_core_owner_state')
        result = run_audit(evidence, plan, process_exit)
        counts = result['memory_by_target'][ip]
        self.assertEqual(counts['baseline_resource_zero_owner_unverified_samples'], 1)
        self.assertEqual(counts['baseline_resource_zero_owner_unhealthy_samples'], 0)
        self.assertFalse(result['completion_audit_pass'])

    def test_memory_uptime_must_remain_above_initial_status_seed(self):
        evidence, plan, process_exit = make_fixture()
        ip = '10.117.152.102'
        first = next(row for row in evidence['memory'] if row['ip'] == ip)
        self.assertGreaterEqual(first['request_started_monotonic'],
                                evidence['before'][ip]['status_request_ended_monotonic'])
        first['data']['uptime_ms'] = 9999
        result = run_audit(evidence, plan, process_exit)
        self.assertIn('memory_uptime_missing_or_decreased:'+ip, result['reasons'])

    def test_directory_output_is_private_and_uses_explicit_run_metadata(self):
        evidence, plan, process_exit = make_fixture()
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / 'run'
            run_dir.mkdir()
            (run_dir / 'evidence.json').write_text(json.dumps(evidence))
            (run_dir / 'run-plan.json').write_text(json.dumps(plan))
            (run_dir / 'process-exit.json').write_text(json.dumps(process_exit))
            (run_dir / 'runner-source.py').write_bytes(SOURCE)
            output = Path(directory) / 'report.json'
            result = AUDIT.audit_directory(run_dir, LABEL, VERSION, output)
            self.assertTrue(result['completion_audit_pass'], result['reasons'])
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            self.assertNotIn('fixture-secret', output.read_text())


if __name__ == '__main__':
    unittest.main()
