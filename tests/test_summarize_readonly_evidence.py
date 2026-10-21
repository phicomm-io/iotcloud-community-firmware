"""Offline tests for the sanitized readonly stress evidence summarizer."""
import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from tests.device.summarize_readonly_evidence import (
    main,
    summarize_evidence,
)


def sample_evidence():
    return {
        'version': 'candidate-x',
        'blockers': [],
        'before': {
            '10.117.152.102': {
                'id': 'private-device-id',
                'state': {'model': 'tc1', 'version': 'candidate-x'},
                'config': {'ssid': 'SECRET_CONFIG_MUST_NOT_LEAK'},
            },
        },
        'rows': [
            {'ip': '10.117.152.102', 'phase': 'soak', 'http': 200,
             'code': 'ok', 'ok': True, 'ms': 10.0,
             'params': {'secret': 'SECRET_ROW_MUST_NOT_LEAK'}},
            {'ip': '10.117.152.102', 'phase': 'soak', 'http': 503,
             'code': None, 'ok': False, 'ms': 3.0},
            {'ip': '10.117.152.102', 'phase': 'soak', 'http': 200,
             'code': 'busy', 'ok': False, 'ms': 4.0},
            {'ip': '10.117.152.102', 'phase': 'soak',
             'error': 'TimeoutError: SECRET_ERROR_MUST_NOT_LEAK'},
            {'ip': '10.117.152.102', 'phase': 'soak', 'http': 200,
             'code': 'unsupported', 'ok': False, 'ms': 2.0},
        ],
        'memory': [
            {'ip': '10.117.152.102', 'phase': 'soak', 'at': 160,
             'http': 200, 'ok': True,
             'data': {'free_memory': 4940, 'json_live_blocks': 0,
                      'memory_active': 0, 'memory_reserved': 0,
                      'memory_waiting': 0, 'memory_denied': 0,
                      'http_tcp_connections': 1}},
            {'ip': '10.117.152.102', 'phase': 'soak', 'at': 100,
             'http': 200, 'ok': True,
             'data': {'free_memory': 5000, 'json_live_blocks': 1,
                      'memory_active': 2, 'memory_reserved': 5,
                      'memory_waiting': 1, 'memory_denied': 0,
                      'http_tcp_connections': 3,
                      'unapproved_secret': 'SECRET_MEMORY_MUST_NOT_LEAK'}},
            {'ip': '10.117.152.102', 'phase': 'soak', 'at': 180,
             'http': 200, 'ok': False,
             'data': {'free_memory': 1, 'json_live_blocks': 999}},
        ],
    }


def complete_run_evidence(mode='full'):
    evidence = sample_evidence()
    full_steps = [
        'concurrency-1', 'concurrency-4', 'concurrency-8', 'concurrency-16',
        'slow-clients', 'aborted-clients', 'buffered-read-clients',
        'missing-read-clients', 'soak',
    ]
    steps = full_steps if mode == 'full' else ['soak']
    phases = [
        {'name': name, 'requested_seconds': 60,
         'clients_per_device': clients}
        for name, clients in (('concurrency-1', 1), ('concurrency-4', 4),
                              ('concurrency-8', 8), ('concurrency-16', 16))
    ] if mode == 'full' else []
    phases.append({'name': 'soak', 'requested_seconds': 14400,
                   'clients_per_device': 1})
    targets = ['10.117.152.111', '10.117.152.102', '10.117.152.110']
    recovery_steps = [
        {'name': phase['name']+'-recovery', 'targets': targets}
        for phase in phases]
    if mode == 'full':
        recovery_steps.extend(
            {'name': name, 'targets': targets}
            for name in ('incomplete-headers-recovery',
                         'aborted-connections-recovery',
                         'buffered-read-input-recovery',
                         'missing-read-eof-recovery'))
    recovery_records = []
    phase_recovery_names = {phase['name']+'-recovery' for phase in phases}
    for planned in recovery_steps:
        name = planned['name']
        groups = [targets] if name in phase_recovery_names else [[ip] for ip in targets]
        for group in groups:
            recovery_records.append({
                'name': name, 'completed': True,
                'required_consecutive_successes': 3,
                'targets': {
                    ip: {'attempts': 3, 'consecutive_successes': 3,
                         'completed': True}
                    for ip in group
                },
            })
    evidence.update({
        'run': {
            'run_id': 'run-123', 'status': 'complete', 'completed': True,
            'plan_completion_required': True,
            'plan': {
                'mode': mode, 'steps': steps,
                'phases': phases,
                'recovery_steps': recovery_steps,
                'targets': [{'ip': ip, 'device_id': 'SECRET_ID'} for ip in targets],
            },
        },
        'scenarios': [{'name': name, 'completed': True, 'elapsed_seconds': 65}
                      for name in steps],
        'phases': [dict(phase, elapsed_seconds=phase['requested_seconds']+.2,
                        completed=True) for phase in phases],
        'recoveries': recovery_records,
        'completion_checks': {
            'network_restore': True,
            'configuration_unchanged': True,
            'idle_quiescence': True,
        },
    })
    return evidence


class SummarizeReadonlyEvidenceTest(unittest.TestCase):
    def test_phase_summary_separates_degradation_categories_and_success_latency(self):
        result = summarize_evidence(sample_evidence())
        phase, = result['model_phase_summary']
        self.assertEqual((phase['model'], phase['phase']), ('tc1', 'soak'))
        self.assertEqual(phase['requests'], 5)
        self.assertEqual(phase['success'], 1)
        self.assertEqual(phase['degraded'], 4)
        self.assertEqual(phase['success_rate'], .2)
        self.assertEqual(phase['http_503'], 1)
        self.assertEqual(phase['busy'], 1)
        self.assertEqual(phase['no_http'], 1)
        self.assertEqual(phase['unexpected'], 1)
        self.assertEqual(phase['success'] + phase['http_503'] + phase['busy']
                         + phase['no_http'] + phase['unexpected'], phase['requests'])
        self.assertLessEqual(phase['success_rate'], 1.0)
        self.assertEqual(phase['success_latency_ms'], {
            'sample_count': 1, 'p50': 10.0, 'p95': 10.0, 'p99': 10.0})

    def test_memory_history_and_descriptive_trends_use_only_allowlisted_metrics(self):
        result = summarize_evidence(sample_evidence())
        self.assertEqual(len(result['memory_history']), 3)
        self.assertEqual(result['memory_history'][0]['metrics']['free_memory'], 5000)
        self.assertNotIn('unapproved_secret', result['memory_history'][0]['metrics'])
        trend, = result['memory_trends']
        self.assertEqual((trend['model'], trend['phase'], trend['samples']),
                         ('tc1', 'soak', 2))
        self.assertEqual((trend['first_at'], trend['last_at']), (100, 160))
        free = trend['metrics']['free_memory']
        self.assertEqual(free['first'], 5000)
        self.assertEqual(free['last'], 4940)
        self.assertEqual(free['delta'], -60)
        self.assertEqual(free['slope_per_minute'], -60)
        self.assertEqual(result['memory_history'][-1]['metrics'], {})
        self.assertEqual(trend['metrics']['json_live_blocks']['maximum'], 1)

    def test_summary_does_not_emit_config_ids_raw_rows_or_errors(self):
        serialized = json.dumps(summarize_evidence(sample_evidence()))
        for secret in ('SECRET_CONFIG_MUST_NOT_LEAK', 'private-device-id',
                       'SECRET_ROW_MUST_NOT_LEAK', 'SECRET_ERROR_MUST_NOT_LEAK',
                       'SECRET_MEMORY_MUST_NOT_LEAK'):
            self.assertNotIn(secret, serialized)
        self.assertNotIn('config', serialized)
        self.assertNotIn('params', serialized)

    def test_completion_is_only_an_explicit_declaration_not_a_pass(self):
        evidence = sample_evidence()
        evidence['blockers'] = ['example blocker']
        snapshot = summarize_evidence(evidence)
        declared = summarize_evidence(evidence, test_complete=True)
        self.assertFalse(snapshot['test_completion']['operator_declared_complete'])
        self.assertTrue(declared['test_completion']['operator_declared_complete'])
        self.assertEqual(declared['test_completion']['evidence_blocker_count'], 1)
        self.assertIn('no leak-free or release-pass', declared['test_completion']['assessment'])
        self.assertNotIn('pass', declared)

    def test_cli_reads_existing_file_and_emits_json_without_network(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence_path = Path(directory) / 'evidence.json'
            evidence_path.write_text(json.dumps(sample_evidence()), encoding='utf-8')
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(main([str(evidence_path)]), 0)
        self.assertEqual(json.loads(output.getvalue())['test_completion']
                         ['operator_declared_complete'], False)

    def test_invalid_evidence_root_is_rejected(self):
        with self.assertRaises(ValueError):
            summarize_evidence([])

    def test_run_review_validates_plan_phase_recovery_and_final_gates(self):
        review = summarize_evidence(complete_run_evidence())['run_review']
        self.assertTrue(review['recorded_complete'])
        self.assertTrue(review['validated_complete'])
        self.assertEqual(review['mode'], 'full')
        self.assertEqual(review['phases'][4]['requested_seconds'], 14400)
        self.assertEqual(review['recoveries'][0]['targets']['10.117.152.102']
                         ['consecutive_successes'], 3)

    def test_run_review_rejects_missing_phase_short_phase_and_early_stop(self):
        evidence = complete_run_evidence()
        evidence['phases'].pop()
        review = summarize_evidence(evidence)['run_review']
        self.assertFalse(review['validated_complete'])
        self.assertIn('phase_missing_or_duplicate:soak', review['reasons'])

        evidence = complete_run_evidence()
        evidence['phases'][4]['elapsed_seconds'] = 100
        review = summarize_evidence(evidence)['run_review']
        self.assertFalse(review['validated_complete'])
        self.assertIn('phase_incomplete_or_short:soak', review['reasons'])

        evidence = complete_run_evidence()
        evidence['phases'][4]['requested_seconds'] = 60
        review = summarize_evidence(evidence)['run_review']
        self.assertFalse(review['validated_complete'])
        self.assertIn('phase_incomplete_or_short:soak', review['reasons'])

        evidence = complete_run_evidence()
        evidence['run'].update(status='interrupted', completed=False)
        review = summarize_evidence(evidence)['run_review']
        self.assertFalse(review['validated_complete'])
        self.assertIn('run_not_recorded_complete', review['reasons'])

        evidence = complete_run_evidence()
        evidence['recoveries'][0]['targets']['10.117.152.111']['consecutive_successes'] = 2
        review = summarize_evidence(evidence)['run_review']
        self.assertFalse(review['validated_complete'])
        self.assertIn('recovery_incomplete:concurrency-1-recovery', review['reasons'])

    def test_run_review_supports_soak_plan_and_multi_target_recovery_records(self):
        evidence = complete_run_evidence('soak-only')
        review = summarize_evidence(evidence)['run_review']
        self.assertTrue(review['validated_complete'])
        self.assertEqual(review['mode'], 'soak-only')

        evidence = complete_run_evidence()
        record = next(row for row in evidence['recoveries']
                      if row['name'] == 'incomplete-headers-recovery')
        evidence['recoveries'].remove(record)
        review = summarize_evidence(evidence)['run_review']
        self.assertFalse(review['validated_complete'])
        self.assertIn('recovery_incomplete:incomplete-headers-recovery', review['reasons'])

        evidence = complete_run_evidence()
        record = next(row for row in evidence['recoveries']
                      if row['name'] == 'incomplete-headers-recovery')
        evidence['recoveries'].append(dict(record))
        review = summarize_evidence(evidence)['run_review']
        self.assertFalse(review['validated_complete'])
        self.assertIn('recovery_incomplete:incomplete-headers-recovery', review['reasons'])

    def test_no_plan_run_is_not_validated_for_release_completion(self):
        evidence = complete_run_evidence()
        evidence['run']['plan_completion_required'] = False
        review = summarize_evidence(evidence)['run_review']
        self.assertFalse(review['validated_complete'])
        self.assertIn('plan_completion_not_required', review['reasons'])

    def test_run_review_does_not_expose_planned_identity(self):
        review = summarize_evidence(complete_run_evidence())['run_review']
        self.assertNotIn('SECRET_ID', json.dumps(review))


if __name__ == '__main__':
    unittest.main()
