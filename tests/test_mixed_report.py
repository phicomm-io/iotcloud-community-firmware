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

    def fixture(self, directory):
        path = directory / 'mixed-evidence.json'
        plan = [{'name': 'mixed-1', 'seconds': 60, 'http_clients': 1, 'mqtt_window': 1}]
        mixed = {'status': 'complete', 'completed': True, 'blockers': [],
                 'version': 'test-version', 'plan': plan,
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


if __name__ == '__main__':
    unittest.main()
