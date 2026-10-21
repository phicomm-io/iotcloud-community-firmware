"""Guard against favourable-looking reports from incomplete or lossy evidence."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from tools import compare_mqtt_http as report


class MixedReportTests(unittest.TestCase):
    def test_counts_do_not_require_latency(self):
        rows = [{'outcome': 'success', 'elapsed_ms': 4},
                {'outcome': 'transport'}, {'outcome': 'timeout', 'elapsed_ms': None},
                {'outcome': 'busy', 'elapsed_ms': 2}, {'outcome': 'cancelled'}]
        result = report.summarize(rows, 2)
        self.assertEqual(result['attempts'], 5)
        self.assertEqual(sum(result['outcomes'].values()), 5)
        self.assertEqual(result['outcomes']['transport'], 1)
        self.assertEqual(result['latency_by_outcome']['transport']['latency_samples'], 0)

    def test_heap_requires_quiet_actual_tail(self):
        clean = {**dict.fromkeys(report.ZERO_FIELDS, 0), 'free_memory': 100}
        rows = [{'ip': 'dut', 'phase': 'final', 'ok': True, 'data': clean} for _ in range(3)]
        evidence = {'memory': rows}
        self.assertEqual(report.heap_median(evidence, 'dut', 'final'), 100)
        evidence = copy.deepcopy(evidence)
        evidence['memory'].append({'ip': 'dut', 'phase': 'final', 'ok': True,
                                   'data': {**clean, 'json_live_blocks': 1}})
        self.assertIsNone(report.heap_median(evidence, 'dut', 'final'))

    def test_excludes_retained_and_unvalidated_telemetry(self):
        valid = {'fresh': True, 'validated': True, 'retained': False,
                 'received_monotonic': 10, 'uptime_source_seconds': 100}
        rows = [valid, {**valid, 'received_monotonic': 15, 'uptime_source_seconds': 105},
                {**valid, 'retained': True, 'received_monotonic': 11},
                {**valid, 'validated': False, 'received_monotonic': 12}]
        result = report.telemetry(rows)
        self.assertEqual(result['validated_live_arrivals'], 2)
        self.assertEqual(result['gap_max_seconds'], 5)
        self.assertEqual(result['source_uptime_span_seconds'], 5)

    def test_new_consumer_timestamp_and_separate_collection_windows(self):
        rows = [{'ip': 'dut', 'phase': 'load', 'collection_stage': stage,
                 'fresh': True, 'validated': True, 'retained': False,
                 'consumer_observed_monotonic': instant,
                 'uptime_source_seconds': instant}
                for stage, instant in [('load', 10), ('load', 15), ('recovery', 40)]]
        metrics = report.phase_metrics({'http': [], 'mqtt': [], 'telemetry': rows},
                                       {}, 'dut', {'name': 'load', 'elapsed_seconds': 60})
        self.assertEqual(metrics['telemetry']['validated_live_arrivals'], 3)
        self.assertEqual(metrics['telemetry']['gap_max_seconds'], 25)
        windows = metrics['telemetry_collection_windows']
        self.assertTrue(windows['separated_in_source'])
        self.assertEqual(windows['load_and_result_drain']['gap_max_seconds'], 5)
        self.assertEqual(windows['recovery']['validated_live_arrivals'], 1)
        legacy = report.phase_metrics({'http': [], 'mqtt': [], 'telemetry': []},
                                      {}, 'dut', {'name': 'load', 'elapsed_seconds': 60})
        self.assertIsNone(legacy['telemetry_collection_windows']['load_and_result_drain'])

    def test_baseline_matching_rejects_drift_and_missing_evidence_without_values(self):
        baseline = {'state': {'requested_fan_on': True, 'requested_speed': 65,
                             'child_lock': False, 'led_enabled': True,
                             'fan_mode': 'auto', 'auto_max_speed': 44, 'applied_speed': 20},
                    'config': {'private': 'must-never-appear'}}
        after = copy.deepcopy(baseline)
        after['state']['applied_speed'] = 44
        self.assertTrue(report.baseline_conditions(baseline, after, 'A1')['matched'])
        after['state']['requested_speed'] = 1
        changed = report.baseline_conditions(baseline, after, 'A1')
        self.assertFalse(changed['matched'])
        self.assertEqual(changed['changed_control_fields'], ['requested_speed'])
        self.assertNotIn('must-never-appear', json.dumps(changed))
        after = copy.deepcopy(baseline)
        after['config']['private'] = 'different-secret'
        self.assertFalse(report.baseline_conditions(baseline, after, 'A1')['configuration_equal'])
        del after['state']['requested_speed']
        self.assertIn('requested_speed', report.baseline_conditions(baseline, after, 'A1')['missing_control_fields'])
        self.assertFalse(report.baseline_conditions({'state': {}}, {'state': {}}, 'TC1')['matched'])
        m1 = {'state': {'brightness_requested': 2}, 'config': {}}
        self.assertTrue(report.baseline_conditions(m1, m1, 'M1')['matched'])

    def fixture(self, directory):
        path = directory / 'mixed-evidence.json'
        plan = [{'name': 'mixed-1', 'seconds': 60, 'http_clients': 1, 'mqtt_window': 1}]
        mixed = {'status': 'complete', 'completed': True, 'blockers': [],
                 'version': 'test-version', 'plan': plan,
                 'profile': 'short', 'http': [], 'mqtt': [], 'telemetry': [],
                 'phases': [{**plan[0], 'completed': True, 'elapsed_seconds': 60}],
                 'targets': {ip: {'device_id': ident, 'model': model.lower()}
                             for ip, (model, ident) in report.MODELS.items()}}
        runner = {'run': {'completed': True}, 'blockers': [],
                  'completion_checks': dict.fromkeys(
                      ('network_restore', 'configuration_unchanged', 'idle_quiescence'), True),
                  'before': {ip: {'id': ident, 'state': {
                      'device_id': ident, 'model': model.lower(), 'version': 'test-version'}}
                      for ip, (model, ident) in report.MODELS.items()}}
        (directory / 'http-runner').mkdir()
        (directory / 'http-runner/evidence.json').write_text(json.dumps(runner))
        path.write_text(json.dumps(mixed))
        return path, mixed

    def _scope_fixture(self, directory):
        path, mixed = self.fixture(directory)
        selected = list(report.MODELS)[:2]
        mixed['targets'] = {ip: mixed['targets'][ip] for ip in selected}
        mixed['selected_targets'] = selected
        mixed['device_status'] = {ip: ('SELECTED' if ip in selected else 'PAUSED')
                                  for ip in report.MODELS}
        runner_path = directory/'http-runner/evidence.json'
        runner = json.loads(runner_path.read_text())
        runner['before'] = {ip: runner['before'][ip] for ip in selected}
        runner_path.write_text(json.dumps(runner))
        path.write_text(json.dumps(mixed))
        target_specs = [ip+'='+report.MODELS[ip][1] for ip in selected]
        return path, mixed, runner, target_specs

    def test_rejects_failed_incomplete_or_mismatched_conditions(self):
        with tempfile.TemporaryDirectory() as temporary:
            path, mixed = self.fixture(Path(temporary))
            report.load(path)
            for change in ({'completed': False}, {'status': 'failed'}, {'blockers': [{}]},
                           {'phases': []}, {'phases': [{**mixed['phases'][0], 'mqtt_window': 4}]},
                           {'phases': [{**mixed['phases'][0], 'elapsed_seconds': 59}]}):
                path.write_text(json.dumps({**mixed, **change}))
                with self.subTest(change=change), self.assertRaises(ValueError):
                    report.load(path)

    def test_two_target_compare_is_end_to_end_scoped_and_marks_m1_paused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            before_dir, after_dir = root/'before', root/'after'
            before_dir.mkdir(); after_dir.mkdir()
            before, _mixed, _runner, target_specs = self._scope_fixture(before_dir)
            after, _mixed, _runner, _ = self._scope_fixture(after_dir)
            result = report.compare(before, after, targets=target_specs)
            self.assertEqual(result['selected_targets'], [ip for ip, _ in report.MODELS.items()
                                                            if ip+'='+report.MODELS[ip][1] in target_specs])
            self.assertEqual(set(result['models']), {'A1', 'TC1'})
            self.assertFalse(result['all_baseline_conditions_matched'], 'missing legacy evidence is not matched')
            self.assertEqual(result['device_status']['10.117.152.110'], 'PAUSED')

    def test_two_target_comparison_rejects_hidden_unselected_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path, mixed, runner, target_specs = self._scope_fixture(directory)
            mixed['telemetry'].append({'ip': '10.117.152.110', 'fresh': True})
            path.write_text(json.dumps(mixed))
            with self.assertRaisesRegex(ValueError, 'unselected device'):
                report.load(path, targets=target_specs)
            mixed['telemetry'].clear()
            path.write_text(json.dumps(mixed))
            runner['memory'] = [{'ip': '10.117.152.110', 'ok': True, 'data': {}}]
            (directory/'http-runner/evidence.json').write_text(json.dumps(runner))
            with self.assertRaisesRegex(ValueError, 'unselected device'):
                report.load(path, targets=target_specs)


if __name__ == '__main__':
    unittest.main()
