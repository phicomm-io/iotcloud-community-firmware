import contextlib
import io
import json
import os
from pathlib import Path
import signal
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from tests.device.readonly_input_edges import main as input_main
from tests.device import readonly_input_edges, readonly_shared_httpd, readonly_http_scheduling
from tests.device.readonly_parallel import (
    covers_expected_records,
    covers_recovery_targets,
    has_complete_targets_and_baselines,
    merge_json_files,
    run_parallel,
)


class FakeRunner:
    def __init__(self, ip, output):
        self.ip = ip
        self.output = Path(output)
        self.output.mkdir(parents=True)
        self.stop = None
        self.blockers = []
        self.rows = []

    def block(self, ip, reason):
        self.blockers.append({'ip': ip, 'reason': reason})
        self.stop.set()

    def save(self):
        path = self.output / 'evidence.json'
        path.write_text(json.dumps({'targets': [[self.ip, 'fake']],
                                    'target_versions': {self.ip: 'v-fake'},
                                    'rows': self.rows,
                                    'blockers': self.blockers,
                                    'baseline': {self.ip: {'id': self.ip}}}))


def _log(log_path, entry):
    payload = (json.dumps(entry) + '\n').encode()
    fd = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.write(fd, payload)
    finally:
        os.close(fd)


def _overlap_worker(runner, log_path):
    ip = runner.ip
    _log(log_path, {'ip': ip, 'step': 'case-start', 'at': time.monotonic()})
    time.sleep(.25)
    runner.rows.append('case')
    _log(log_path, {'ip': ip, 'step': 'recovery-start', 'at': time.monotonic()})
    time.sleep(.05)
    runner.rows.append('recovery')
    _log(log_path, {'ip': ip, 'step': 'done', 'at': time.monotonic()})
    runner.save()


def _block_worker(runner, _):
    if runner.ip.endswith('.1'):
        runner.block(runner.ip, 'fake blocker')
        runner.save()
    else:
        runner.stop.wait(5)
        runner.rows.append('cancelled')
        runner.save()


def _exception_worker(runner, _):
    raise RuntimeError('fake child exception')


def _abrupt_worker(runner, _):
    if runner.ip.endswith('.1'):
        # Reproduce the old shared Event failure: a dead worker leaves its
        # Condition semaphore acquired. SharedStop never touches this object.
        runner.poison_event._cond.acquire()
        _log(runner.marker, {'ip': runner.ip, 'pid': os.getpid()})
        os._exit(7)
    runner.stop.wait(5)


def _self_sigterm_worker(runner, marker):
    _log(marker, {'ip': runner.ip,
                  'default_handler': signal.getsignal(signal.SIGTERM) == signal.SIG_DFL})
    os.kill(os.getpid(), signal.SIGTERM)


class AggregateRunner:
    def __init__(self, ip, output, evidence):
        self.ip = ip
        self.output = Path(output)
        self.output.mkdir(parents=True)
        self.evidence = evidence


def _write_aggregate_evidence(runner):
    for name, content in runner.evidence.items():
        (runner.output / name).write_text(json.dumps(content))


def _target_evidence(ip):
    return {'targets': [[ip, 'fake-' + ip.rsplit('.', 1)[-1]]],
            'target_versions': {ip: 'v-' + ip.rsplit('.', 1)[-1]},
            'baseline': {ip: {'device_id': 'fake-' + ip.rsplit('.', 1)[-1]}}}


def _recovery(ip, name, successes=3):
    return {'name': name, 'completed': successes >= 3,
            'targets': {ip: {'completed': successes >= 3,
                             'consecutive_successes': successes}}}


def _hang_worker(runner, marker):
    if runner.ip.endswith('.1'):
        runner.block(runner.ip, 'trigger cancellation')
        runner.save()
        return
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    _log(marker, {'ip': runner.ip, 'pid': os.getpid()})
    while True:
        time.sleep(.05)


def _parent_signal_worker(runner, marker):
    _log(marker, {'ip': runner.ip, 'pid': os.getpid()})
    runner.stop.wait(5)
    runner.save()


def _ignore_term_worker(runner, marker):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    _log(marker, {'ip': runner.ip, 'pid': os.getpid()})
    while True:
        time.sleep(.05)


def _signal_controller(root, marker):
    runners = [FakeRunner(f'10.0.1.{index}', Path(root) / str(index))
               for index in (1, 2)]
    try:
        run_parallel(runners, lambda runner: _parent_signal_worker(runner, marker), 3)
    except KeyboardInterrupt:
        return


def _repeated_signal_controller(root, marker):
    import tests.device.readonly_parallel as parallel

    runners = [FakeRunner(f'10.0.2.{index}', Path(root) / str(index))
               for index in (1, 2)]
    with patch.object(parallel, 'TERMINATE_JOIN_SECONDS', .3), \
            patch.object(parallel, 'KILL_JOIN_SECONDS', .3):
        try:
            run_parallel(runners, lambda runner: _ignore_term_worker(runner, marker), 3)
        except KeyboardInterrupt:
            return


class ReadonlyParallelTests(unittest.TestCase):
    def test_workers_overlap_preserve_per_device_order_and_merge_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log_path = root / 'events.jsonl'
            runners = [FakeRunner(f'10.0.0.{index}', root / str(index))
                       for index in range(1, 4)]
            result = run_parallel(runners, lambda runner: _overlap_worker(runner, log_path), 3)
            self.assertTrue(result['completed'], result)
            for pid in result['worker_pids']:
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            rows = [json.loads(line) for line in log_path.read_text().splitlines()]
            starts = {row['ip']: row['at'] for row in rows if row['step'] == 'case-start'}
            recoveries = {row['ip']: row['at'] for row in rows
                          if row['step'] == 'recovery-start'}
            self.assertEqual(len(starts), 3)
            self.assertLess(max(starts.values()), min(recoveries.values()))
            for runner in runners:
                own = [row['step'] for row in rows if row['ip'] == runner.ip]
                self.assertEqual(own, ['case-start', 'recovery-start', 'done'])
                self.assertEqual(runner.rows, [])
            merged = merge_json_files(
                [runner.output / 'evidence.json' for runner in runners],
                root / 'combined.json', list_keys=('rows', 'blockers'),
                dict_keys=('baseline', 'target_versions'),
                metadata={'device_workers': 3})
            self.assertEqual(merged['targets'], [[runner.ip, 'fake'] for runner in runners])
            self.assertEqual(merged['rows'], ['case', 'recovery'] * 3)
            self.assertEqual(set(merged['baseline']), {runner.ip for runner in runners})
            self.assertEqual(set(merged['target_versions']),
                             {runner.ip for runner in runners})
            self.assertEqual((root / 'combined.json').stat().st_mode & 0o777, 0o600)

    def test_cancelled_sibling_is_blocked_and_cannot_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            runners = [FakeRunner(f'10.0.0.{index}', Path(tmp) / str(index))
                       for index in (1, 2)]
            result = run_parallel(runners, lambda runner: _block_worker(runner, None), 3)
            self.assertFalse(result['completed'])
            for runner in runners:
                evidence = json.loads((runner.output / 'evidence.json').read_text())
                self.assertTrue(evidence['blockers'])

    def test_cancel_timeout_kills_unresponsive_child(self):
        import tests.device.readonly_parallel as parallel

        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'hung-worker.jsonl'
            runners = [FakeRunner(f'10.0.0.{index}', Path(tmp) / str(index))
                       for index in (1, 2)]
            with patch.object(parallel, 'CANCEL_JOIN_SECONDS', .1), \
                    patch.object(parallel, 'TERMINATE_JOIN_SECONDS', .1), \
                    patch.object(parallel, 'KILL_JOIN_SECONDS', .1):
                result = run_parallel(
                    runners, lambda runner: _hang_worker(runner, marker), 3)
            self.assertFalse(result['completed'])
            self.assertIn('cancellation deadline', result['failure'])
            for pid in result['worker_pids']:
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)

    def test_parent_sigterm_stops_and_reaps_workers(self):
        import multiprocessing

        for received_signal in (signal.SIGTERM, signal.SIGINT):
            with self.subTest(signal=received_signal), tempfile.TemporaryDirectory() as tmp:
                marker = Path(tmp) / 'signal-worker.jsonl'
                controller = multiprocessing.get_context('fork').Process(
                    target=_signal_controller, args=(tmp, marker))
                controller.start()
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    if marker.exists() and len(marker.read_text().splitlines()) == 2:
                        break
                    time.sleep(.02)
                self.assertTrue(marker.exists())
                worker_pids = [json.loads(line)['pid']
                               for line in marker.read_text().splitlines()]
                self.assertEqual(len(worker_pids), 2)
                os.kill(controller.pid, received_signal)
                controller.join(10)
                if controller.is_alive():
                    controller.kill()
                    controller.join(3)
                self.assertFalse(controller.is_alive())
                self.assertEqual(controller.exitcode, 0)
                for pid in worker_pids:
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)
                controller.close()

    def test_repeated_sigterm_during_cleanup_does_not_interrupt_reap(self):
        import multiprocessing

        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'repeated-signal-workers.jsonl'
            controller = multiprocessing.get_context('fork').Process(
                target=_repeated_signal_controller, args=(tmp, marker))
            controller.start()
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if marker.exists() and len(marker.read_text().splitlines()) == 2:
                    break
                time.sleep(.02)
            rows = [json.loads(line) for line in marker.read_text().splitlines()]
            worker_pids = [row['pid'] for row in rows]
            os.kill(controller.pid, signal.SIGTERM)
            time.sleep(.05)
            os.kill(controller.pid, signal.SIGTERM)
            controller.join(5)
            if controller.is_alive():
                controller.kill()
                controller.join(1)
            self.assertFalse(controller.is_alive())
            self.assertEqual(controller.exitcode, 0)
            for pid in worker_pids:
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            controller.close()

    def test_sigterm_after_fork_before_start_returns_reaps_registered_child(self):
        import multiprocessing.process
        import tests.device.readonly_parallel as parallel

        with tempfile.TemporaryDirectory() as tmp:
            runners = [FakeRunner(f'10.0.3.{index}', Path(tmp) / str(index))
                       for index in (1, 2)]
            started_pids = []
            original_start = multiprocessing.process.BaseProcess.start
            sent_signal = False

            def start_then_signal(process):
                nonlocal sent_signal
                original_start(process)
                started_pids.append(process.pid)
                if not sent_signal:
                    sent_signal = True
                    os.kill(os.getpid(), signal.SIGTERM)

            with patch.object(multiprocessing.process.BaseProcess, 'start',
                              start_then_signal), \
                    patch.object(parallel, 'TERMINATE_JOIN_SECONDS', .1), \
                    patch.object(parallel, 'KILL_JOIN_SECONDS', .3):
                with self.assertRaises(KeyboardInterrupt):
                    run_parallel(runners, lambda runner: _ignore_term_worker(
                        runner, Path(tmp) / 'start-window.jsonl'), 3)
            self.assertEqual(len(started_pids), 1)
            with self.assertRaises(ProcessLookupError):
                os.kill(started_pids[0], 0)

    def test_missing_case_or_device_baseline_fails_aggregate_gate(self):
        expected_ips = {'10.0.0.1', '10.0.0.2'}
        evidence = {
            'targets': [['10.0.0.1', 'id-1'], ['10.0.0.2', 'id-2']],
            'baseline': {'10.0.0.1': {'device_id': 'id-1'},
                         '10.0.0.2': {'device_id': 'id-2'}},
        }
        self.assertTrue(has_complete_targets_and_baselines(
            evidence, expected_ips, 'baseline'))
        self.assertFalse(covers_expected_records(
            [{'ip': '10.0.0.1', 'name': 'case-a'}],
            {(ip, name) for ip in expected_ips for name in ('case-a', 'case-b')},
            ('ip', 'name'), exact=True))
        del evidence['baseline']['10.0.0.2']
        self.assertFalse(has_complete_targets_and_baselines(
            evidence, expected_ips, 'baseline'))

    def test_block_and_child_exception_stop_all_workers(self):
        for execute in (_block_worker, _exception_worker):
            with self.subTest(execute=execute.__name__), tempfile.TemporaryDirectory() as tmp:
                runners = [FakeRunner(f'10.0.0.{index}', Path(tmp) / str(index))
                           for index in (1, 2)]
                result = run_parallel(runners, lambda runner: execute(runner, None), 3)
                self.assertFalse(result['completed'])
                self.assertTrue(result['failure'])

    def test_abrupt_child_exit_cannot_poison_parent_stop_flag(self):
        import multiprocessing

        with tempfile.TemporaryDirectory() as tmp:
            runners = [FakeRunner(f'10.0.0.{index}', Path(tmp) / str(index))
                       for index in (1, 2)]
            poison_event = multiprocessing.get_context('fork').Event()
            for runner in runners:
                runner.marker = Path(tmp) / 'abrupt.jsonl'
                runner.poison_event = poison_event
            started = time.monotonic()
            result = run_parallel(runners, _abrupt_worker, 3)
            self.assertFalse(result['completed'])
            self.assertLess(time.monotonic() - started, 3)
            self.assertTrue(result['failure'])
            for pid in result['worker_pids']:
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)

    def test_worker_resets_inherited_signal_handler_before_callback(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'signal-workers.jsonl'
            runners = [FakeRunner(f'10.0.0.{index}', Path(tmp) / str(index))
                       for index in (1, 2)]
            result = run_parallel(runners,
                                  lambda runner: _self_sigterm_worker(runner, marker), 3)
            self.assertFalse(result['completed'])
            rows = [json.loads(line) for line in marker.read_text().splitlines()]
            self.assertEqual(len(rows), 2)
            self.assertTrue(all(row['default_handler'] for row in rows))
            for pid in result['worker_pids']:
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)

    def test_real_aggregators_assign_recovery_targets_and_reject_incomplete_reads(self):
        aggregators = ('input', 'shared', 'scheduling')
        for device_count in (2, 3):
            for aggregator in aggregators:
                with self.subTest(devices=device_count, aggregator=aggregator), \
                        tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    ips = [f'10.0.0.{index}' for index in range(1, device_count + 1)]
                    runners = []
                    input_scenarios = {
                        ip: [SimpleNamespace(name='case-a'), SimpleNamespace(name='case-b')]
                        for ip in ips}
                    for ip in ips:
                        base = _target_evidence(ip)
                        recoveries = []
                        if aggregator == 'input':
                            base.update(scenarios=[{'ip': ip, 'name': item.name}
                                                   for item in input_scenarios[ip]],
                                        recoveries=[])
                            for item in input_scenarios[ip]:
                                recoveries.append(_recovery(ip, item.name + '-recovery'))
                            recoveries.append(_recovery(ip, 'final-recovery'))
                            base['recoveries'] = recoveries
                            content = {'evidence.json': base}
                        elif aggregator == 'shared':
                            scenarios = readonly_shared_httpd.validate_scenarios(
                                readonly_shared_httpd.build_scenarios(ip))
                            base.update(http_cases=[{'ip': ip, 'name': item.name}
                                                    for item in scenarios],
                                        recoveries=[])
                            base['recoveries'] = [
                                *[_recovery(ip, item.name + '-recovery')
                                  for item in scenarios],
                                _recovery(ip, 'final-recovery')]
                            content = {'shared-httpd-evidence.json': base}
                        else:
                            base.pop('baseline')
                            base.update(run={'completed': True},
                                        before={ip: {'id': 'fake-' + ip.rsplit('.', 1)[-1],
                                                    'state': {'uptime_seconds':
                                                              int(ip.rsplit('.', 1)[-1])}}},
                                        phases=[], recoveries=[], scenarios=[], rows=[],
                                        memory=[], summary={})
                            cases = [{'ip': ip, 'round': 1, 'phase': phase}
                                     for phase in readonly_http_scheduling.CASES]
                            sched = {'targets': base['targets'],
                                     'target_versions': base['target_versions'],
                                     'cases': cases,
                                     'recovery': [{'ip': ip, 'round': 1,
                                                   'recovered': True,
                                                   'reads': 3,
                                                   'consecutive_successes': 3}],
                                     'blockers': []}
                            content = {'evidence.json': base,
                                       'http-scheduling.json': sched}
                        runners.append(AggregateRunner(ip, root / ip.rsplit('.', 1)[-1],
                                                       content))
                    result = run_parallel(runners, _write_aggregate_evidence, 3)
                    self.assertTrue(result['completed'], result)
                    if aggregator == 'input':
                        complete, evidence = readonly_input_edges.aggregate_parallel_evidence(
                            runners, {ip: 'fake' for ip in ips}, input_scenarios,
                            result, root / 'merged-input.json')
                    elif aggregator == 'shared':
                        complete, evidence = readonly_shared_httpd.aggregate_parallel_evidence(
                            runners, {ip: 'fake' for ip in ips}, result,
                            root / 'merged-shared.json')
                    else:
                        complete, evidence, scheduling = \
                            readonly_http_scheduling.aggregate_parallel_evidence(
                                runners, {ip: 'fake' for ip in ips}, 1, result,
                                root / 'merged-scheduling.json',
                                root / 'merged-scheduling-run.json')
                        self.assertEqual({row['ip'] for row in scheduling['recovery']},
                                         set(ips))
                    self.assertTrue(complete, aggregator)
                    self.assertEqual(set(evidence.get('baseline', evidence.get('before'))),
                                     set(ips))
                    self.assertEqual(evidence['target_versions'],
                                     {ip: 'v-' + ip.rsplit('.', 1)[-1] for ip in ips})
                    baseline_key = 'baseline' if aggregator != 'scheduling' else 'before'
                    for ip in ips:
                        row = evidence[baseline_key][ip]
                        if aggregator == 'scheduling':
                            self.assertEqual(row['id'], 'fake-' + ip.rsplit('.', 1)[-1])
                            self.assertEqual(row['state']['uptime_seconds'],
                                             int(ip.rsplit('.', 1)[-1]))
                        else:
                            self.assertEqual(row['device_id'],
                                             'fake-' + ip.rsplit('.', 1)[-1])
                    if aggregator != 'scheduling':
                        recoveries = evidence['recoveries']
                        expected_recoveries = {(ip, row['name']) for ip in ips
                                               for row in recoveries
                                               if row['name'] != 'final-recovery'}
                        self.assertTrue(covers_recovery_targets(recoveries,
                                                               expected_recoveries))
                        damaged_runner = runners[0]
                        damaged_filename = ('evidence.json' if aggregator == 'input'
                                            else 'shared-httpd-evidence.json')
                        damaged = damaged_runner.evidence[damaged_filename]
                        target_record = next(row for row in damaged['recoveries']
                                             if row['name'].endswith('-recovery'))
                        target_result = next(iter(target_record['targets'].values()))
                        target_result['consecutive_successes'] = 2
                        target_result['completed'] = False
                        target_record['completed'] = False
                        _write_aggregate_evidence(damaged_runner)
                        if aggregator == 'input':
                            incomplete, _ = readonly_input_edges.aggregate_parallel_evidence(
                                runners, {ip: 'fake' for ip in ips}, input_scenarios,
                                result, root / 'incomplete-input.json')
                        else:
                            incomplete, _ = readonly_shared_httpd.aggregate_parallel_evidence(
                                runners, {ip: 'fake' for ip in ips}, result,
                                root / 'incomplete-shared.json')
                        self.assertFalse(incomplete)
                    else:
                        damaged_runner = runners[0]
                        scheduling_data = damaged_runner.evidence['http-scheduling.json']
                        scheduling_data['recovery'][0]['recovered'] = False
                        damaged_runner.evidence['evidence.json']['run']['completed'] = False
                        _write_aggregate_evidence(damaged_runner)
                        incomplete, _, _ = readonly_http_scheduling.aggregate_parallel_evidence(
                            runners, {ip: 'fake' for ip in ips}, 1, result,
                            root / 'incomplete-scheduling.json',
                            root / 'incomplete-scheduling-run.json')
                        self.assertFalse(incomplete)

    def test_default_cli_dry_run_keeps_single_worker_and_no_network(self):
        output = io.StringIO()
        args = ['--target', '10.117.152.111=b0f893303134', '--version', 'fake']
        with contextlib.redirect_stdout(output):
            status = input_main(args)
        self.assertEqual(status, 0)
        manifest = json.loads(output.getvalue())
        self.assertEqual(manifest['mode'], 'dry-run')
        self.assertEqual(manifest['device_workers'], 1)


if __name__ == '__main__':
    unittest.main()
