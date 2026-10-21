"""Offline tests for the bounded HTTP/MQTT mixed runner."""
import base64
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest import mock
import subprocess
import sys

from tests.device import mqtt_http_mixed as MIXED
from tests.device import mqtt_mixed_controller as CONTROLLER


class FakePublishInfo:
    rc = 0

    def __init__(self, published=True, wait_hook=None):
        self.published = published
        self.wait_hook = wait_hook

    def wait_for_publish(self, timeout=None):
        if self.wait_hook is not None:
            self.wait_hook(timeout)

    def is_published(self):
        return self.published


def _state(ip, version):
    ident, model = MIXED.TARGETS[ip]
    result = {
        'device_id': ident,
        'model': model,
        'version': version,
        'uptime_seconds': 1001,
        'wifi_connected': True,
        'mqtt_connected': True,
        'watchdog_state': 'healthy',
    }
    controls = {
        'a1': {'requested_fan_on': True, 'requested_speed': 20,
               'child_lock': False, 'led_enabled': True},
        'tc1': {'socket_states': [False] * 6, 'child_lock': False,
                'led_enabled': True},
        'm1': {'brightness_requested': 70},
    }
    result.update(controls[model])
    return result


class FakeRunner:
    instances = []

    def __init__(self, targets, version, output, require_shared_core=False):
        self.targets = targets
        self.version = version
        self.output = Path(output)
        self.stop = threading.Event()
        self.rows = []
        self.lock = threading.Lock()
        self.blockers = []
        self.memory = []
        self.before = {}
        self.run = {'completed': False}
        self.completion_checks = {
            'network_restore': False,
            'configuration_unchanged': False,
            'idle_quiescence': False,
        }
        self.check_calls = []
        self.recover_calls = []
        self.memory_windows = []
        self.save_count = 0
        self.finish_calls = 0
        self.instances.append(self)

    def initialize(self):
        for ip, ident in self.targets.items():
            state = _state(ip, self.version)
            state['device_id'] = ident
            self.before[ip] = {
                'id': ident,
                'state': state,
                'config': {'mqtt_host': 'broker', 'mqtt_port': 1883},
            }

    def block(self, ip, reason):
        self.blockers.append({'ip': ip, 'reason': reason})
        self.stop.set()

    def check(self, ip, state, start, end=None):
        self.check_calls.append((ip, state.get('uptime_seconds'), start, end))

    def sample(self, ip, phase, params=None):
        row = {
            'ip': ip, 'phase': phase, 'action': 'status.get',
            'ok': True, 'http': 200, 'code': 'ok', 'ms': 1.0,
            'error': None, 'request_started_monotonic': time.monotonic(),
            'request_ended_monotonic': time.monotonic(),
        }
        row.update({k: self.before[ip]['state'][k] for k in
                    ('wifi_connected', 'mqtt_connected')})
        self.rows.append(row)
        return row

    def memory_window(self, phase):
        self.memory_windows.append(phase)

    def memory_sample(self, ip, phase):
        self.memory.append({'ip': ip, 'phase': phase, 'ok': True,
                            'data': {'mqtt_pending': 0}})

    def recover(self, phase, seconds=45):
        self.recover_calls.append(phase)
        for ip in self.targets:
            for _ in range(3):
                self.sample(ip, phase)

    def finish(self):
        self.finish_calls += 1
        self.completion_checks = {key: True for key in self.completion_checks}
        self.run['completed'] = True

    def save_interrupted(self, exc):
        self.run.update(status='failed', completed=False)
        self.run['failure'] = type(exc).__name__

    def save(self):
        self.save_count += 1


class FakeMQTTClient:
    instances = []

    def __init__(self, host, port, client_id, username=None, password=None,
                 timeout=5, evidence_dir=None, state_mode='object', publish_mode='normal'):
        self.host, self.port = host, port
        self.client_id = client_id
        self.messages = MIXED.BoundedMessageQueue(maximum=MIXED.MESSAGE_QUEUE_LIMIT)
        self.connected = threading.Event()
        self.connected.set()
        self.disconnected = False
        self.state_mode = state_mode
        self.publish_mode = publish_mode
        self.state = None
        self.root = None
        self.client = self
        self.publish_count = 0
        self.published_requests = []
        self.response_codes = None
        self.result_mutator = None
        self.result_topic_override = None
        self.result_retained = False
        self.raw_override = None
        self.publish_ack_hook = None
        self.instances.append(self)

    def subscribe(self, topic):
        if topic.endswith('/state'):
            self.root = topic[:-len('/state')]
            self.messages.put((topic, self.state, True, b'{}'))
            self.messages.put((topic, self.state, False,
                               json.dumps(self.state).encode()))

    def publish(self, topic, payload, qos=0, retain=False):
        if self.publish_mode == 'raise':
            raise OSError('fake publish failure')
        request = json.loads(payload)
        self.published_requests.append((topic, request, qos, retain))
        self.publish_count += 1
        self.last_qos, self.last_retain = qos, retain
        response_code = (self.response_codes.pop(0) if self.response_codes else None)
        if response_code == 'busy':
            body = {'request_id': request['request_id'], 'code': 'busy'}
        elif response_code is not None:
            state = dict(self.state)
            state['uptime_seconds'] += 1
            body = {'request_id': request['request_id'], 'code': response_code,
                    'data': state}
        elif self.state_mode == 'nonobject-result':
            body = None
        elif self.publish_mode in ('busy-always', 'busy-first') and (
                self.publish_mode == 'busy-always' or self.publish_count <= 6):
            body = {'request_id': request['request_id'], 'code': 'busy'}
        else:
            state = dict(self.state)
            state['uptime_seconds'] += 1
            body = {'request_id': request['request_id'], 'code': 'ok', 'data': state}
        if self.publish_mode == 'unmatched' and body is not None:
            body['request_id'] = 'readonly-mixed-unmatched'
        if self.result_mutator is not None and body is not None:
            body = self.result_mutator(body)
        suppress_result = ((self.publish_mode == 'timeout-first' and self.publish_count == 1) or
                           self.publish_mode == 'silent')
        if not suppress_result:
            self.messages.put((self.result_topic_override or self.root+'/result', body,
                               self.result_retained,
                               self.raw_override if self.raw_override is not None else
                               (json.dumps(body).encode() if body is not None else b'not-json')))
        published = self.publish_mode != 'no-puback'
        if self.publish_mode == 'puback-first' and self.publish_count == 1:
            published = False
        return FakePublishInfo(published=published, wait_hook=self.publish_ack_hook)

    def disconnect(self):
        self.disconnected = True
        self.connected.clear()


def _recovery_runner(ip, version):
    runner = FakeRunner({ip: TARGETS[ip]}, version, '')
    runner.initialize()
    return runner


def _recovery_fixture(directory, response_codes=None):
    runner = FakeRunner(TARGETS, VERSION, directory)
    runner.initialize()
    clients, roots = {}, {}
    for ip, (_ident, model) in MIXED.TARGETS.items():
        root = 'phicomm/user/'+model+'/'+TARGETS[ip]
        client = FakeMQTTClient('broker', 1883, 'offline-'+model)
        client.state = _state(ip, VERSION)
        client.root = root
        if response_codes and ip in response_codes:
            client.response_codes = list(response_codes[ip])
        clients[ip], roots[ip] = client, root
    evidence = MIXED.Evidence(Path(directory)/'recovery.json', 'offline', VERSION,
                              'short', [])
    return runner, clients, roots, evidence


class FakeHelpers:
    @staticmethod
    def load_credentials():
        return {'host': 'broker', 'port': 1883, 'username': 'user', 'password': 'secret'}

    @staticmethod
    def validate_broker_config(_config, _credentials):
        return True


def fake_runtime(state_mode='object', publish_mode='normal'):
    class RuntimeMQTT(FakeMQTTClient):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, state_mode=state_mode,
                             publish_mode=publish_mode, **kwargs)
            ip = next(ip for ip, (_ident, model) in MIXED.TARGETS.items()
                      if '-'+model+'-' in args[2])
            self.state = _state(ip, VERSION)

    def loader():
        return (FakeRunner, FakeHelpers, RuntimeMQTT,
                lambda username, model, ident: 'phicomm/'+username+'/'+model+'/'+ident,
                lambda username, model, ident: username+'-'+model+'-'+ident)
    return loader


VERSION = '0.9.15-testmixed'
TARGETS = {ip: ident for ip, (ident, _model) in MIXED.TARGETS.items()}


class HTTPResponseGapTests(unittest.TestCase):
    @staticmethod
    def row(ended, http=200, code='ok', ok=True, **extra):
        return dict(ip='target', request_ended_monotonic=ended,
                    http=http, code=code, ok=ok, **extra)

    def test_busy_and_occasional_failures_separate_response_from_success(self):
        clock = mock.Mock(return_value=0.0)
        gap = MIXED.HTTPResponseGap(['target'], clock())
        gap.observe([self.row(10), self.row(20, http=503, code=None, ok=False),
                     self.row(29, http=503, code='busy', ok=False)])
        clock.return_value = 40.0
        row = gap.snapshot(clock())['target']
        self.assertEqual(row['last_response_gap_seconds'], 11)
        self.assertEqual(row['last_success_gap_seconds'], 30)
        self.assertEqual(row['maximum_response_gap_seconds'], 19)
        self.assertLess(row['maximum_response_gap_seconds'], MIXED.HTTP_RESPONSE_GAP_LIMIT)

    def test_silence_reaches_bound_despite_failed_attempts(self):
        clock = mock.Mock(return_value=30.0)
        gap = MIXED.HTTPResponseGap(['target'], 0)
        gap.observe([self.row(29, ok=False, http=None, code=None, error='TimeoutError')])
        self.assertEqual(gap.snapshot(clock())['target']['maximum_response_gap_seconds'], 30)

    def test_late_response_does_not_hide_gap_and_out_of_order_does_not_regress(self):
        gap = MIXED.HTTPResponseGap(['target'], 0)
        gap.observe([self.row(31), self.row(10)])
        row = gap.snapshot(32)['target']
        self.assertEqual(row['last_response_gap_seconds'], 1)
        self.assertEqual(row['maximum_response_gap_seconds'], 31)


class NetworkReadinessTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.runner = FakeRunner(TARGETS, VERSION, self.directory.name)
        self.runner.initialize()
        self.evidence = MIXED.Evidence(Path(self.directory.name)/'network.json',
                                      'offline', VERSION, 'short', [])
        self.clock = [0.0]
        self.monitor = MIXED.NetworkReadiness(self.runner, self.evidence,
                                             monotonic=lambda: self.clock[0])
        self.runner.mixed_network_readiness = self.monitor
        self.ip = next(iter(TARGETS))
        self.good = _state(self.ip, VERSION)

    def observe(self, ready, *, started=None, can_restore=True, source='mqtt-correlated',
                phase='first'):
        data = dict(self.good, mqtt_connected=ready)
        self.monitor.observe(self.ip, data, phase, source, started,
                             can_restore=can_restore)

    def test_short_false_then_fresh_true_records_safe_fields_and_recovers(self):
        self.clock[0] = 1
        self.observe(False)
        self.clock[0] = 5
        self.observe(True, started=2)
        self.assertFalse(self.runner.stop.is_set())
        state = self.evidence.data['network_readiness'][self.ip]
        self.assertIsNone(state['degraded_since_monotonic'])
        self.assertEqual(state['maximum_not_ready_seconds'], 4)
        self.assertEqual(state['current_not_ready_seconds'], 0)
        self.assertEqual([row['event'] for row in self.evidence.data['network_events']],
                         ['network-degraded', 'network-ready'])
        self.assertIs(self.evidence.data['network_events'][0]['mqtt_connected'], False)
        validated = MIXED._validate_device_data(dict(self.good, mqtt_connected=False),
                                               self.ip, self.good, VERSION)
        self.assertIs(validated['mqtt_connected'], False)
        self.assertIn('uptime_seconds', validated)

    def test_strict_bool_missing_and_nonbool_are_protocol_errors(self):
        for key in ('wifi_connected', 'mqtt_connected'):
            for value in (None, 0, 1, 'true', [], {}):
                with self.subTest(key=key, value=value):
                    data = dict(self.good, **{key: value})
                    with self.assertRaisesRegex(ValueError, 'must be boolean'):
                        MIXED._validate_device_data(data, self.ip, self.good, VERSION)
            data = dict(self.good)
            del data[key]
            with self.assertRaisesRegex(ValueError, 'must be boolean'):
                MIXED._validate_device_data(data, self.ip, self.good, VERSION)

    def test_identity_control_uptime_and_watchdog_remain_immediate_errors(self):
        control = next(key for key in self.good if key in
                       ('requested_speed', 'socket_states', 'brightness_requested'))
        mutations = [('device_id', 'wrong'), ('model', 'wrong'), ('version', 'wrong'),
                     ('uptime_seconds', 0), ('uptime_seconds', float('nan')),
                     ('watchdog_state', 'stalled'), (control, 'changed')]
        for key, value in mutations:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                MIXED._validate_device_data(dict(self.good, **{key: value}),
                                           self.ip, self.good, VERSION)

    def test_telemetry_true_and_overlapping_old_request_cannot_clear_degradation(self):
        self.clock[0] = 1
        self.observe(False)
        self.clock[0] = 10
        self.observe(True, can_restore=False, source='mqtt-telemetry')
        self.observe(True, started=.5)
        self.assertEqual(self.monitor.state[self.ip]['degraded_since_monotonic'], 1)
        self.clock[0] = 31
        self.monitor.check('later-phase')
        self.assertTrue(self.runner.stop.is_set())
        self.assertTrue(self.monitor.state[self.ip]['expired'])

    def test_other_device_ready_does_not_clear_target_degradation(self):
        self.observe(False)
        other_ip = next(ip for ip in TARGETS if ip != self.ip)
        self.clock[0] = 10
        self.monitor.observe(other_ip, _state(other_ip, VERSION), 'next-phase',
                             'mqtt-correlated', started=5, can_restore=True)
        self.clock[0] = 30
        self.monitor.check('next-phase')
        self.assertTrue(self.monitor.state[self.ip]['expired'])
        self.assertFalse(self.monitor.state[other_ip]['expired'])

    def test_late_true_cannot_erase_historical_overrun(self):
        self.observe(False)
        self.clock[0] = 31
        self.observe(True, started=30, phase='recovery')
        self.assertTrue(self.runner.stop.is_set())
        state = self.evidence.data['network_readiness'][self.ip]
        self.assertTrue(state['expired'])
        self.assertEqual(state['maximum_not_ready_seconds'], 31)
        self.assertEqual(state['degraded_since_monotonic'], 0)
        self.assertNotIn('network-ready', [row['event'] for row in self.evidence.data['network_events']])

    def test_phase_end_and_busy_unrelated_messages_do_not_restart_deadline(self):
        self.observe(False)
        self.clock[0] = 20
        self.monitor.check('first-end')
        self.assertFalse(self.runner.stop.is_set())
        client = FakeMQTTClient('broker', 1883, 'network-test', publish_mode='busy-always')
        client.state, client.root = self.good, 'offline/network'
        client.messages.put(('unrelated/state', self.good, False, b'{}'))
        worker = MIXED.DeviceMQTTWorker(self.ip, client, client.root, self.runner,
                                        self.evidence, threading.Event())
        worker.run_window('next-phase', time.monotonic()+.08, 1, .001)
        self.assertTrue(any(row['outcome'] == 'busy' for row in self.evidence.data['mqtt']))
        self.assertEqual(self.monitor.state[self.ip]['degraded_since_monotonic'], 0)
        self.clock[0] = 30
        self.monitor.check('next-phase-end')
        self.assertTrue(self.runner.stop.is_set())
        self.assertEqual(self.evidence.data['network_readiness'][self.ip]['maximum_not_ready_seconds'], 30)

    def test_unresolved_degradation_cannot_pass_final_or_recovery_boundary(self):
        self.observe(False)
        self.clock[0] = 1
        self.monitor.require_ready('final-network-gate')
        self.assertTrue(self.runner.stop.is_set())
        self.assertIn('Unresolved network degradation', self.runner.blockers[0]['reason'])

    def test_existing_http_samples_observe_false_and_fresh_true_without_extra_io(self):
        observed_type = MIXED._network_observed_runner(FakeRunner)
        runner = observed_type(TARGETS, VERSION, self.directory.name)
        runner.initialize()
        runner.mixed_network_readiness = MIXED.NetworkReadiness(
            runner, self.evidence, monotonic=lambda: self.clock[0])
        runner.before[self.ip]['state']['mqtt_connected'] = False
        self.clock[0] = 1
        runner.sample(self.ip, 'load')
        self.clock[0] = 5
        runner.before[self.ip]['state']['mqtt_connected'] = True
        runner.sample(self.ip, 'recovery')
        self.assertFalse(runner.stop.is_set())
        self.assertIsNone(runner.mixed_network_readiness.state[self.ip]['degraded_since_monotonic'])
        self.assertEqual(len(runner.rows), 2)
        runner.before[self.ip]['state']['wifi_connected'] = 1
        runner.sample(self.ip, 'bad-bool')
        self.assertTrue(runner.stop.is_set())
        self.assertIn('Invalid HTTP status network fields', runner.blockers[0]['reason'])

    def test_correlated_short_false_then_true_passes_load_without_control_failure(self):
        client = FakeMQTTClient('broker', 1883, 'network-transient')
        client.state, client.root = self.good, 'offline/network'
        client.result_mutator = lambda body: {
            **body, 'data': dict(body['data'], mqtt_connected=client.publish_count > 1)}
        worker = MIXED.DeviceMQTTWorker(self.ip, client, client.root, self.runner,
                                        self.evidence, threading.Event())
        worker.run_window('transient', time.monotonic()+.1, 1, .001)
        self.assertIsNone(worker.failure)
        self.assertFalse(self.runner.stop.is_set())
        self.assertIsNone(self.monitor.state[self.ip]['degraded_since_monotonic'])
        outcomes = self.evidence.data['mqtt']
        self.assertIs(outcomes[0]['mqtt_connected'], False)
        self.assertTrue(any(row.get('mqtt_connected') is True for row in outcomes[1:]))

    def test_correlated_missing_bool_blocks_immediately(self):
        for key in ('wifi_connected', 'mqtt_connected'):
            runner = FakeRunner(TARGETS, VERSION, self.directory.name)
            runner.initialize()
            client = FakeMQTTClient('broker', 1883, 'network-invalid')
            client.state, client.root = self.good, 'offline/network'
            client.result_mutator = lambda body, missing=key: {
                **body, 'data': {k: v for k, v in body['data'].items() if k != missing}}
            worker = MIXED.DeviceMQTTWorker(self.ip, client, client.root, runner,
                                            self.evidence, threading.Event())
            worker.run_window('invalid', time.monotonic()+.1, 1, .001)
            self.assertIn('must be boolean', worker.failure)
            self.assertTrue(runner.stop.is_set())
            self.assertEqual(client.publish_count, 1)

    def test_pure_monitor_bounds_blocked_recovery_and_thread_is_joined(self):
        clock = [0.0]
        reached = []
        class BlockedRecovery(FakeRunner):
            def recover(inner, *_args, **_kwargs):
                clock[0] = 31
                reached.append(inner.stop.wait(.5))

        loader = fake_runtime()
        def runtime():
            _base, helpers, client, root, client_id = loader()
            return BlockedRecovery, helpers, client, root, client_id

        monitor_type = MIXED.NetworkReadiness
        def make_monitor(runner, evidence):
            return monitor_type(runner, evidence, monotonic=lambda: clock[0])
        def phase(runner, _clients, _roots, _evidence, _plan, _profile):
            runner.mixed_network_readiness.observe(
                self.ip, dict(self.good, mqtt_connected=False), 'phase-end', 'mqtt-correlated')
            return None
        with mock.patch.object(MIXED, 'NetworkReadiness', side_effect=make_monitor), \
                mock.patch.object(MIXED, 'run_phase', side_effect=phase):
            with self.assertRaisesRegex(RuntimeError, 'recovery failed'):
                MIXED.execute(TARGETS, VERSION, 'blocked-recovery', 'short',
                              Path(self.directory.name)/'blocked-run', runtime_loader=runtime,
                              plan_override=[dict(name='phase', seconds=.1)], run_timeout=2)
        self.assertEqual(reached, [True])
        self.assertFalse(any(thread.name == 'mixed-network-monitor'
                             for thread in threading.enumerate()))
        saved = json.loads((Path(self.directory.name)/'blocked-run'/'mixed-evidence.json').read_text())
        self.assertTrue(saved['network_readiness'][self.ip]['expired'])
        self.assertEqual(saved['network_readiness'][self.ip]['maximum_not_ready_seconds'], 31)
        self.assertFalse(saved['completed'])

    def test_recovery_cannot_clear_elapsed_deadline(self):
        runner, clients, roots, evidence = _recovery_fixture(self.directory.name)
        monitor = MIXED.NetworkReadiness(runner, evidence, monotonic=lambda: self.clock[0])
        runner.mixed_network_readiness = monitor
        monitor.observe(self.ip, dict(self.good, mqtt_connected=False), 'phase-end', 'mqtt-correlated')
        self.clock[0] = 30
        with self.assertRaisesRegex(RuntimeError, 'HTTP recovery'):
            MIXED.recover(runner, clients, roots, evidence, 'next-recovery')
        self.assertTrue(monitor.state[self.ip]['expired'])
        self.assertTrue(runner.stop.is_set())
        self.assertTrue(all(not client.published_requests for client in clients.values()))


class MixedRunnerTests(unittest.TestCase):
    def setUp(self):
        FakeMQTTClient.instances.clear()

    def test_http_silence_stops_soak_but_does_not_change_short_gate(self):
        for profile in ('soak', 'short'):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as directory:
                runner = FakeRunner(TARGETS, VERSION, directory)
                runner.initialize()
                clients = {ip: FakeMQTTClient('broker', 1883, 'offline-'+ip)
                           for ip in TARGETS}
                roots = {ip: 'phicomm/offline/'+ip for ip in TARGETS}
                for ip, client in clients.items():
                    client.root = roots[ip]
                    client.state = _state(ip, VERSION)
                evidence = MIXED.Evidence(Path(directory)/'evidence.json',
                                          'offline', VERSION, profile, [])
                plan = dict(name='http-silence', seconds=.15, http_clients=1,
                            mqtt_window=1, mqtt_period=.01)
                # A deterministic fake clock for HTTP gap observations only;
                # real thread deadlines remain independent and bounded.
                original_snapshot = MIXED.HTTPResponseGap.snapshot
                def expired_snapshot(monitor, _now):
                    started = min(state['response'] for state in monitor.state.values())
                    return original_snapshot(monitor, started+30)
                with mock.patch.object(MIXED, '_http_worker', return_value=None), \
                        mock.patch.object(MIXED.HTTPResponseGap, 'snapshot', expired_snapshot):
                    MIXED.run_phase(runner, clients, roots, evidence, plan, profile)
                row = evidence.data['phases'][0]
                self.assertEqual(runner.stop.is_set(), profile == 'soak')
                if profile == 'soak':
                    self.assertIn('HTTP response gap', row['abort_reason'])
                    self.assertFalse(row['completed'])
                    self.assertEqual(set(row['http_response_liveness']), set(TARGETS))
                    self.assertTrue(all(item['stop_reason'] for item in row['http_response_liveness'].values()))
                else:
                    self.assertNotIn('http_response_liveness', row)
                    self.assertIn('No successful HTTP', row['abort_reason'])

    def test_authorized_targets_and_command_guard(self):
        specs = [ip+'='+ident for ip, ident in TARGETS.items()]
        self.assertEqual(MIXED.validate_targets(specs), TARGETS)
        with self.assertRaises(ValueError):
            MIXED.validate_targets(['10.117.152.1=unknown'])
        payload = MIXED.make_status_payload('readonly-mixed-abc')
        self.assertIs(MIXED.guard_status_payload(payload), payload)
        payload['action'] = 'config.set'
        with self.assertRaises(ValueError):
            MIXED.guard_status_payload(payload)

    def test_classification_retained_and_protocol_errors(self):
        result_topic, state_topic = 'root/result', 'root/state'
        body = {'request_id': 'readonly-mixed-id', 'code': 'ok', 'data': {}}
        self.assertEqual(MIXED.classify_mqtt_message(
            result_topic, body, False, body['request_id'], result_topic, state_topic), 'success')
        self.assertEqual(MIXED.classify_mqtt_message(
            result_topic, body, True, body['request_id'], result_topic, state_topic),
            'retained_result')
        self.assertEqual(MIXED.classify_mqtt_message(
            state_topic, body, True, body['request_id'], result_topic, state_topic),
            'retained_telemetry')

    def test_bounded_message_queue_drops_oldest_and_counts(self):
        dropped = []
        messages = MIXED.BoundedMessageQueue(maximum=2, on_drop=lambda: dropped.append(True))
        messages.put('one')
        messages.put('two')
        messages.put('three')
        self.assertEqual(messages.dropped, 1)
        self.assertEqual(messages.unfinished_tasks, 2)
        self.assertEqual(dropped, [True])
        self.assertEqual(messages.get_nowait(), 'two')
        messages.task_done()
        self.assertEqual(messages.get_nowait(), 'three')
        messages.task_done()
        self.assertEqual(messages.unfinished_tasks, 0)

    def test_recovery_matching_topic_invalid_json_is_protocol_error(self):
        client = FakeMQTTClient('broker', 1883, 'offline')
        client.messages.put(('root/result', None, False, b'not-json'))
        with self.assertRaises(MIXED.MQTTProtocolError):
            MIXED.receive_correlated(
                client, 'readonly-mixed-id', 'root/result', 'root/state',
                0.1, [], '10.117.152.111', 'recovery',
                _recovery_runner('10.117.152.111', VERSION))

    def test_recover_retries_only_matching_busy_and_keeps_success_contract(self):
        response_codes = {ip: ['busy', 'ok'] for ip in TARGETS}
        sleeps = []
        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(directory, response_codes)
            MIXED.recover(runner, clients, roots, evidence, 'busy-recovery',
                          sleep=sleeps.append)
            saved = json.loads((Path(directory)/'recovery.json').read_text())
        recovery = evidence.data['recovery'][0]
        self.assertTrue(recovery['completed'])
        self.assertEqual(len(recovery['mqtt_results']), 3)
        self.assertEqual(len(evidence.data['recovery_attempts']), 3)
        self.assertEqual(len(saved['recovery_attempts']), 3)
        self.assertTrue(all(row['outcome'] == 'success' for row in recovery['mqtt_results']))
        self.assertTrue(all(row['attempt_count'] == 2 and row['busy_attempt_count'] == 1
                            for row in recovery['mqtt_results']))
        all_ids = [request_id for row in recovery['mqtt_results']
                   for request_id in row['attempt_request_ids']]
        self.assertEqual(len(all_ids), len(set(all_ids)))
        self.assertTrue(all(json.loads(base64.b64decode(row['raw_json_base64']))['code'] == 'busy'
                            for row in evidence.data['recovery_attempts']))
        self.assertTrue(all(client.last_qos == 1 and client.last_retain is False
                            for client in clients.values()))
        self.assertEqual(sleeps, [MIXED.RECOVERY_BUSY_RETRY_DELAY]*3)

    def test_recover_persistent_busy_is_bounded_and_persists_each_rejection(self):
        ip = '10.117.152.111'
        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['busy']*5})
            with self.assertRaisesRegex(RuntimeError, 'exhausted matching busy'):
                MIXED.recover(runner, clients, roots, evidence, 'persistent-busy',
                              sleep=lambda _seconds: None)
            saved = json.loads((Path(directory)/'recovery.json').read_text())
        attempts = saved['recovery_attempts']
        self.assertEqual(len(clients[ip].published_requests), 4)
        self.assertEqual(len(attempts), 4)
        self.assertEqual([row['attempt'] for row in attempts], [1, 2, 3, 4])
        self.assertTrue(all(row['outcome'] == 'busy' for row in attempts))
        self.assertEqual(len({row['request_id'] for row in attempts}), 4)
        self.assertTrue(attempts[-1]['retry_exhausted'])
        self.assertTrue(runner.blockers)

    def test_recover_nonbusy_error_is_not_retried(self):
        ip = '10.117.152.111'
        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['unavailable']})
            with self.assertRaisesRegex(RuntimeError, 'failed correlation'):
                MIXED.recover(runner, clients, roots, evidence, 'nonbusy-recovery')
        self.assertEqual(len(clients[ip].published_requests), 1)
        self.assertEqual(evidence.data['recovery_attempts'], [])
        self.assertTrue(runner.blockers)

    def test_recover_busy_oversleep_updates_persisted_attempt_without_duplicate(self):
        ip = '10.117.152.111'
        clock = [0.0]

        def oversleep(_seconds):
            clock[0] = 16.0

        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['busy']})
            with self.assertRaises(TimeoutError):
                MIXED.recover(runner, clients, roots, evidence, 'oversleep-recovery',
                              monotonic=lambda: clock[0], sleep=oversleep)
            saved = json.loads((Path(directory)/'recovery.json').read_text())
        attempts = saved['recovery_attempts']
        self.assertEqual(len(clients[ip].published_requests), 1)
        self.assertEqual(len(attempts), 1)
        self.assertTrue(attempts[0]['deadline_exhausted'])
        self.assertFalse(attempts[0]['retry_exhausted'])

    def test_recover_late_puback_and_identity_errors_do_not_retry(self):
        ip = '10.117.152.111'
        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['busy']})
            clock = [0.0]
            clients[ip].publish_ack_hook = lambda _timeout: clock.__setitem__(0, 16.0)
            with self.assertRaises(TimeoutError):
                MIXED.recover(runner, clients, roots, evidence, 'late-puback',
                              monotonic=lambda: clock[0], sleep=lambda _seconds: None)
        self.assertEqual(len(clients[ip].published_requests), 1)
        self.assertEqual(evidence.data['recovery_attempts'], [])
        self.assertTrue(runner.blockers)

        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['ok']})
            clients[ip].result_mutator = lambda body: {
                **body, 'data': {**body['data'], 'device_id': 'wrong-device'}}
            with self.assertRaisesRegex(RuntimeError, 'state validation'):
                MIXED.recover(runner, clients, roots, evidence, 'bad-identity')
        self.assertEqual(len(clients[ip].published_requests), 1)
        self.assertTrue(runner.blockers)

        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['ok']})

            def block_on_check(target_ip, *_args):
                runner.block(target_ip, 'synthetic uptime/control gate')

            runner.check = block_on_check
            with self.assertRaisesRegex(RuntimeError, 'state gate'):
                MIXED.recover(runner, clients, roots, evidence, 'blocked-check')
        self.assertEqual(len(clients[ip].published_requests), 1)
        self.assertTrue(runner.blockers)

        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['ok']})
            clients[ip].result_mutator = lambda body: {
                **body, 'data': {**body['data'], 'wifi_connected': False}}
            with self.assertRaisesRegex(RuntimeError, 'state gate'):
                MIXED.recover(runner, clients, roots, evidence, 'network-not-ready')
        self.assertEqual(len(clients[ip].published_requests), 1)
        self.assertTrue(runner.blockers)

    def test_recover_retained_or_wrong_topic_busy_does_not_retry(self):
        ip = '10.117.152.111'
        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['busy']})
            clients[ip].result_retained = True
            with self.assertRaisesRegex(RuntimeError, 'failed correlation'):
                MIXED.recover(runner, clients, roots, evidence, 'retained-busy')
        self.assertEqual(len(clients[ip].published_requests), 1)
        self.assertEqual(evidence.data['recovery_attempts'], [])

    def test_recover_busy_requires_valid_raw_json(self):
        ip = '10.117.152.111'
        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['busy']})
            clients[ip].raw_override = b'{"request_id":"different","code":"busy"}'
            with self.assertRaisesRegex(MIXED.MQTTProtocolError, 'raw validation'):
                MIXED.recover(runner, clients, roots, evidence, 'invalid-busy-raw')
        self.assertEqual(len(clients[ip].published_requests), 1)
        self.assertEqual(evidence.data['recovery_attempts'], [])
        self.assertTrue(evidence.data['protocol_errors'])
        self.assertTrue(runner.blockers)

        with tempfile.TemporaryDirectory() as directory:
            runner, clients, roots, evidence = _recovery_fixture(
                directory, {ip: ['busy']})
            clients[ip].result_topic_override = 'wrong/result'
            with mock.patch.object(MIXED, 'MQTT_RESULT_TIMEOUT', 0.02):
                with self.assertRaises(TimeoutError):
                    MIXED.recover(runner, clients, roots, evidence, 'wrong-topic')
        self.assertEqual(len(clients[ip].published_requests), 1)
        self.assertEqual(evidence.data['recovery_attempts'], [])

    def test_missing_diagnostics_remain_unavailable(self):
        result = MIXED.optional_diagnostics([{'ok': True, 'data': {'mqtt_pending': 0}}])
        self.assertEqual(result['queue.mqtt_pending']['availability'], 'available')
        self.assertEqual(result['queue.mqtt_pending']['last'], 0)
        self.assertEqual(result['queue.mqtt_queue_bytes']['availability'], 'NOT_AVAILABLE')
        self.assertEqual(result['queue.mqtt_peak']['availability'], 'NOT_AVAILABLE')
        self.assertEqual(result['stack.mqtt_stack_watermark']['availability'], 'NOT_AVAILABLE')

    def test_new_flattened_mqtt_diagnostics_are_available(self):
        fields = {name: index for index, name in enumerate(MIXED.QUEUE_FIELDS)}
        result = MIXED.optional_diagnostics([{'ok': True, 'data': fields}])
        self.assertEqual(len(MIXED.QUEUE_FIELDS), 22)
        self.assertEqual(result['queue.mqtt_queue_bytes']['last'],
                         fields['mqtt_queue_bytes'])
        self.assertEqual(result['queue.mqtt_tx_buffer_bytes']['availability'], 'available')
        for name in ('mqtt_protocol_depth', 'mqtt_operation', 'mqtt_last_error_stage',
                     'mqtt_last_error', 'mqtt_errors', 'mqtt_reconnects',
                     'mqtt_max_queue_delay_ms'):
            self.assertEqual(result['queue.'+name]['last'], fields[name])

    def test_telemetry_is_separated_by_collection_stage_and_missing_is_na(self):
        rows = [
            {'ip': '10.117.152.111', 'phase': 'phase-a', 'collection_stage': 'load',
             'retained': True, 'validated': False},
            {'ip': '10.117.152.111', 'phase': 'phase-a', 'collection_stage': 'load',
             'retained': False, 'validated': True},
            {'ip': '10.117.152.111', 'phase': 'phase-a', 'collection_stage': 'recovery',
             'retained': False, 'validated': True},
        ]
        load = MIXED._telemetry_observation(rows, '10.117.152.111', 'phase-a', 'load')
        recovery = MIXED._telemetry_observation(rows, '10.117.152.111', 'phase-a', 'recovery')
        unavailable = MIXED._telemetry_observation([], '10.117.152.111', 'phase-a', 'load')
        self.assertEqual(load['availability'], 'observed')
        self.assertEqual(load['validated_nonretained_count'], 1)
        self.assertEqual(load['retained_excluded_count'], 1)
        self.assertEqual(recovery['validated_nonretained_count'], 1)
        self.assertEqual(recovery['observed_within_phase_window_count'], 0)
        self.assertEqual(recovery['observed_after_phase_window_count'], 1)
        self.assertEqual(unavailable['availability'], 'NOT_AVAILABLE')
        self.assertFalse(load['source_timestamp_available'])

    def test_unexpected_initialized_runtime_exception_runs_finish_and_stays_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            old_private = MIXED.PRIVATE
            clients_before = len(FakeMQTTClient.instances)
            MIXED.PRIVATE = Path(directory)
            try:
                output = Path(directory)/'run'
                with mock.patch.object(FakeRunner, 'memory_window',
                                       side_effect=RuntimeError('synthetic memory failure')):
                    with self.assertRaisesRegex(RuntimeError, 'synthetic memory failure'):
                        MIXED.execute(
                            TARGETS, VERSION, 'unexpected-after-init', 'short', output,
                            runtime_loader=fake_runtime(), plan_override=[], run_timeout=3)
                evidence = json.loads((output/'mixed-evidence.json').read_text())
                runner = FakeRunner.instances[-1]
                clients = FakeMQTTClient.instances[clients_before:]
            finally:
                MIXED.PRIVATE = old_private
        self.assertEqual(evidence['status'], 'failed')
        self.assertFalse(evidence['completed'])
        self.assertEqual(evidence['failure_type'], 'RuntimeError')
        self.assertEqual(runner.finish_calls, 1)
        self.assertFalse(runner.run['completed'])
        self.assertTrue(evidence['exception_finalization']['attempted'])
        self.assertTrue(evidence['exception_finalization']['completed'])
        self.assertTrue(clients)
        self.assertTrue(all(client.disconnected for client in clients))

    def test_outcome_latency_stays_separated_by_category(self):
        rows = [
            {'outcome': 'success', 'elapsed_ms': 10.0},
            {'outcome': 'success', 'elapsed_ms': 30.0},
            {'outcome': 'busy', 'elapsed_ms': 2.0},
            {'outcome': 'timeout', 'elapsed_ms': 12000.0},
            {'outcome': 'transport', 'elapsed_ms': 1.0},
            {'outcome': 'transport'},
            {'outcome': 'cancelled', 'code': 'phase_stopped'},
        ]
        summary = MIXED.summarize_latencies(rows)
        self.assertEqual(summary['success']['count'], 2)
        self.assertEqual(summary['success']['latency_samples'], 2)
        self.assertEqual(summary['success']['p50_ms'], 20.0)
        self.assertEqual(summary['busy']['count'], 1)
        self.assertEqual(summary['timeout']['p99_ms'], 12000.0)
        self.assertEqual(summary['transport']['count'], 2)
        self.assertEqual(summary['transport']['latency_samples'], 1)
        self.assertEqual(summary['cancelled']['count'], 1)
        self.assertEqual(sum(row['count'] for row in summary.values()), len(rows))

    def test_correlated_busy_is_liveness_without_success(self):
        ip = '10.117.152.111'
        runner = FakeRunner({ip: TARGETS[ip]}, VERSION, '')
        runner.initialize()
        client = FakeMQTTClient('broker', 1883, 'busy-test', publish_mode='busy-always')
        client.state = _state(ip, VERSION)
        client.root = 'phicomm/user/a1/device'
        with tempfile.TemporaryDirectory() as directory:
            evidence = MIXED.Evidence(Path(directory)/'evidence.json',
                                      'offline', VERSION, 'short', [])
            stop = threading.Event()
            worker = MIXED.DeviceMQTTWorker(ip, client, client.root, runner, evidence, stop)
            started = time.monotonic()
            with mock.patch.object(MIXED, 'MQTT_NO_SUCCESS_LIMIT', 0.15):
                worker.run_window('busy-only', started+0.4, 1, 0.001)
        self.assertIsNone(worker.phase_abort_reason)
        self.assertFalse(stop.is_set())
        self.assertGreater(worker.last_correlated_response, worker.phase_started)
        self.assertEqual(worker.last_success, worker.phase_started)
        self.assertTrue(evidence.data['mqtt'])
        self.assertTrue(all(row['outcome'] == 'busy' for row in evidence.data['mqtt']))

    def test_silent_phase_cancels_pending_and_still_runs_final_gates(self):
        plan = [{'name': 'silent-mixed', 'http_clients': 1, 'mqtt_window': 1,
                 'seconds': 0.2, 'mqtt_period': 0.01}]
        with tempfile.TemporaryDirectory() as directory:
            old_private = MIXED.PRIVATE
            MIXED.PRIVATE = Path(directory)
            try:
                output = Path(directory)/'run'

                def recovery_stub(runner, _clients, _roots, evidence, phase):
                    evidence.data['recovery'].append({
                        'phase': phase, 'completed': True,
                        'http_completed': True, 'mqtt_results': [],
                    })

                with mock.patch.object(MIXED, 'MQTT_NO_SUCCESS_LIMIT', 0.15), \
                        mock.patch.object(MIXED, 'recover', recovery_stub):
                    with self.assertRaisesRegex(RuntimeError, 'incomplete'):
                        MIXED.execute(
                            TARGETS, VERSION, 'silent-final-gates', 'short', output,
                            runtime_loader=fake_runtime(publish_mode='silent'),
                            plan_override=plan, run_timeout=3)
                evidence = json.loads((output/'mixed-evidence.json').read_text())
                runner = FakeRunner.instances[-1]
            finally:
                MIXED.PRIVATE = old_private
        self.assertEqual(evidence['status'], 'failed')
        self.assertFalse(evidence['completed'])
        self.assertTrue(evidence['recovery'][0]['completed'])
        self.assertEqual(runner.finish_calls, 1)
        self.assertFalse(runner.run['completed'])
        self.assertTrue(runner.completion_checks['idle_quiescence'])
        self.assertTrue(any(row['outcome'] == 'cancelled' for row in evidence['mqtt']))
        self.assertFalse(any(row['outcome'] == 'timeout' and
                             row.get('code') == 'phase_stopped' for row in evidence['mqtt']))

    def test_short_and_soak_plans_are_bounded_and_read_only(self):
        short = MIXED.phase_plan('short')
        soak = MIXED.phase_plan('soak')
        self.assertEqual([p['http_clients'] for p in short], [1, 4, 8, 16])
        self.assertEqual([p['mqtt_window'] for p in short], [1, 2, 4, 4])
        self.assertEqual(soak[0]['seconds'], 14400)
        self.assertEqual(soak[0]['http_clients'], 4)
        self.assertEqual(soak[0]['mqtt_period'], 1.0)
        self.assertTrue(all(p['mqtt_window'] <= 4 for p in short+soak))

    def test_fake_runtime_exercises_phase_recovery_and_finish(self):
        plan = [{'name': 'fake-mixed', 'http_clients': 1, 'mqtt_window': 2,
                 'seconds': 0.08, 'mqtt_period': 0.02}]
        specs = [ip+'='+ident for ip, ident in TARGETS.items()]
        with tempfile.TemporaryDirectory() as directory:
            old_private = MIXED.PRIVATE
            MIXED.PRIVATE = Path(directory)
            try:
                output = Path(directory)/'run'
                result = MIXED.execute(
                    TARGETS, VERSION, 'offline-test', 'short', output,
                    runtime_loader=fake_runtime(), plan_override=plan,
                    run_timeout=5,
                )
            finally:
                MIXED.PRIVATE = old_private
        self.assertTrue(result['completed'])
        self.assertEqual(result['status'], 'complete')
        self.assertFalse(any(thread.name == 'mixed-network-monitor'
                             for thread in threading.enumerate()))
        self.assertEqual(result['phases'][0]['name'], 'fake-mixed')
        self.assertTrue(result['phases'][0]['completed'])
        self.assertEqual(result['recovery'][0]['completed'], True)
        self.assertEqual(len(result['recovery'][0]['mqtt_results']), 3)
        self.assertTrue(all(row['outcome'] == 'success' for row in result['mqtt']))
        self.assertTrue(all(row['qos'] == 1 and row['retain'] is False for row in result['mqtt']))
        self.assertTrue(any(row.get('retained') is True and row.get('fresh') is False
                            for row in result['telemetry']))
        fresh = [row for row in result['telemetry'] if row.get('fresh') is True]
        self.assertTrue(fresh)
        self.assertTrue(all(row['uptime_reset_check'] ==
                            'not_evaluated_without_source_timestamp' for row in fresh))
        recovery_fresh = [row for row in result['telemetry']
                          if row.get('phase') == 'fake-mixed' and row.get('fresh')]
        self.assertTrue(recovery_fresh)
        self.assertTrue(all(row.get('validated') is True for row in recovery_fresh))
        self.assertEqual(len(FakeRunner.instances[-1].check_calls),
                         len(result['mqtt']) + len(result['recovery'][0]['mqtt_results']))
        self.assertEqual(result['queue_drops'][0]['per_device'], {ip: 0 for ip in TARGETS})
        self.assertEqual(FakeRunner.instances[-1].memory_windows, ['baseline'])
        self.assertTrue(FakeRunner.instances[-1].check_calls)
        self.assertEqual(FakeMQTTClient.instances[0].last_qos, 1)
        self.assertTrue(all(client.disconnected for client in FakeMQTTClient.instances))

    def test_phase_checkpoints_runner_and_partial_evidence(self):
        plan = [{'name': 'checkpoint-mixed', 'http_clients': 1,
                 'mqtt_window': 2, 'seconds': 0.08, 'mqtt_period': 0.02}]
        with mock.patch.object(MIXED, 'PROGRESS_CHECKPOINT_INTERVAL', 0.5):
            with tempfile.TemporaryDirectory() as directory:
                old_private = MIXED.PRIVATE
                MIXED.PRIVATE = Path(directory)
                try:
                    result = MIXED.execute(
                        TARGETS, VERSION, 'checkpoint-test', 'short', Path(directory)/'run',
                        runtime_loader=fake_runtime(), plan_override=plan, run_timeout=5)
                finally:
                    MIXED.PRIVATE = old_private
        checkpoints = result['phases'][0]['progress_checkpoints']
        self.assertTrue(checkpoints)
        self.assertTrue(all(row['completed'] is False for row in checkpoints))
        self.assertTrue(all('http_rows' in row and 'mqtt_rows' in row for row in checkpoints))
        self.assertGreater(FakeRunner.instances[-1].save_count, 0)

    def test_malformed_correlated_result_is_saved_as_protocol_blocker(self):
        plan = [{'name': 'bad-mixed', 'http_clients': 1, 'mqtt_window': 1,
                 'seconds': 0.05, 'mqtt_period': 0.02}]
        with tempfile.TemporaryDirectory() as directory:
            old_private = MIXED.PRIVATE
            MIXED.PRIVATE = Path(directory)
            try:
                output = Path(directory)/'run'
                with self.assertRaises(RuntimeError):
                    MIXED.execute(
                        TARGETS, VERSION, 'offline-protocol', 'short', output,
                        runtime_loader=fake_runtime('nonobject-result'),
                        plan_override=plan, run_timeout=5,
                    )
                evidence = json.loads((output/'mixed-evidence.json').read_text())
            finally:
                MIXED.PRIVATE = old_private
        self.assertEqual(evidence['status'], 'failed')
        self.assertFalse(evidence['completed'])
        self.assertTrue(evidence['protocol_errors'])
        self.assertTrue(evidence['blockers'])

    def _execute_failure(self, label, runtime, plan=None):
        plan = plan or [{'name': 'failure-mixed', 'http_clients': 1,
                         'mqtt_window': 1, 'seconds': 0.04, 'mqtt_period': 0.01}]
        with tempfile.TemporaryDirectory() as directory:
            old_private = MIXED.PRIVATE
            MIXED.PRIVATE = Path(directory)
            try:
                output = Path(directory)/'run'
                with self.assertRaises((RuntimeError, TimeoutError, OSError)):
                    MIXED.execute(TARGETS, VERSION, label, 'short', output,
                                  runtime_loader=runtime, plan_override=plan,
                                  run_timeout=3)
                evidence = json.loads((output/'mixed-evidence.json').read_text())
                return evidence, FakeRunner.instances[-1]
            finally:
                MIXED.PRIVATE = old_private

    def test_unmatched_result_does_not_skip_pending_expiry(self):
        plan = [{'name': 'unmatched-mixed', 'http_clients': 1, 'mqtt_window': 1,
                 'seconds': 0.4, 'mqtt_period': 0.02}]
        with mock.patch.object(MIXED, 'MQTT_RESULT_TIMEOUT', 0.15):
            evidence, runner = self._execute_failure(
                'unmatched', fake_runtime(publish_mode='unmatched'), plan)
        self.assertTrue(evidence['unmatched_mqtt'])
        self.assertTrue(any(row['outcome'] == 'timeout' for row in evidence['mqtt']))
        self.assertTrue(runner.blockers)
        self.assertEqual(evidence['status'], 'failed')

    def test_publish_exception_is_blocked_and_persisted(self):
        evidence, runner = self._execute_failure('publish-error', fake_runtime(publish_mode='raise'))
        self.assertTrue(runner.blockers)
        self.assertEqual(evidence['status'], 'failed')
        self.assertTrue(any(row['outcome'] == 'transport' for row in evidence['mqtt']))

    def test_puback_timeout_is_recorded_without_immediate_block(self):
        evidence, runner = self._execute_failure('puback-timeout', fake_runtime(publish_mode='no-puback'))
        self.assertTrue(runner.blockers)
        self.assertEqual(evidence['status'], 'failed')
        self.assertTrue(any(row['outcome'] == 'timeout' for row in evidence['mqtt']))

    def test_single_result_timeout_can_recover_and_complete_phase(self):
        plan = [{'name': 'result-timeout-recovery', 'http_clients': 1,
                 'mqtt_window': 1, 'seconds': 0.14, 'mqtt_period': 0.01}]
        with mock.patch.object(MIXED, 'MQTT_RESULT_TIMEOUT', 0.03), \
                tempfile.TemporaryDirectory() as directory:
            old_private = MIXED.PRIVATE
            MIXED.PRIVATE = Path(directory)
            try:
                result = MIXED.execute(
                    TARGETS, VERSION, 'result-timeout-recovery', 'short',
                    Path(directory)/'run', runtime_loader=fake_runtime(publish_mode='timeout-first'),
                    plan_override=plan, run_timeout=5)
            finally:
                MIXED.PRIVATE = old_private
        self.assertTrue(result['completed'])
        self.assertEqual(result['status'], 'complete')
        self.assertFalse(FakeRunner.instances[-1].blockers)
        outcomes = [row['outcome'] for row in result['mqtt']]
        self.assertIn('timeout', outcomes)
        self.assertIn('success', outcomes)

    def test_single_puback_timeout_can_recover_and_complete_phase(self):
        plan = [{'name': 'puback-timeout-recovery', 'http_clients': 1,
                 'mqtt_window': 1, 'seconds': 0.14, 'mqtt_period': 0.01}]
        with tempfile.TemporaryDirectory() as directory:
            old_private = MIXED.PRIVATE
            MIXED.PRIVATE = Path(directory)
            try:
                result = MIXED.execute(
                    TARGETS, VERSION, 'puback-timeout-recovery', 'short',
                    Path(directory)/'run', runtime_loader=fake_runtime(publish_mode='puback-first'),
                    plan_override=plan, run_timeout=5)
            finally:
                MIXED.PRIVATE = old_private
        self.assertTrue(result['completed'])
        self.assertFalse(FakeRunner.instances[-1].blockers)
        self.assertTrue(any(row['outcome'] == 'timeout' for row in result['mqtt']))
        self.assertTrue(any(row['outcome'] == 'success' for row in result['mqtt']))

    def test_queue_overflow_blocks_immediately_and_records_event(self):
        with mock.patch.object(MIXED, 'MESSAGE_QUEUE_LIMIT', 1):
            evidence, runner = self._execute_failure('queue-overflow', fake_runtime())
        self.assertTrue(runner.blockers)
        self.assertTrue(evidence['queue_overflow_events'])
        self.assertEqual(evidence['status'], 'failed')

    def test_uncaught_worker_exception_is_blocked(self):
        def explode(*_args, **_kwargs):
            raise RuntimeError('fake worker defect')
        with mock.patch.object(MIXED.DeviceMQTTWorker, 'run_window', explode):
            evidence, runner = self._execute_failure('worker-error', fake_runtime())
        self.assertTrue(runner.blockers)
        self.assertTrue(any('Unhandled MQTT worker exception' in row['reason']
                            for row in runner.blockers))
        self.assertEqual(evidence['status'], 'failed')

    def test_hard_deadline_stops_stalled_mqtt_worker(self):
        def stall(self, *_args, **_kwargs):
            time.sleep(0.12)
        plan = [{'name': 'deadline-mixed', 'http_clients': 1,
                 'mqtt_window': 1, 'seconds': 0.01, 'mqtt_period': 0.01}]
        with mock.patch.object(MIXED, 'MQTT_RESULT_TIMEOUT', 0.01), \
                mock.patch.object(MIXED, 'MQTT_PUBACK_TIMEOUT', 0.01), \
                mock.patch.object(MIXED, 'HTTP_REQUEST_TIMEOUT', 0.01), \
                mock.patch.object(MIXED, 'PHASE_SHUTDOWN_GRACE', 0.01), \
                mock.patch.object(MIXED.DeviceMQTTWorker, 'run_window', stall):
            evidence, runner = self._execute_failure('hard-deadline', fake_runtime(), plan)
        self.assertTrue(any('hard deadline' in row['reason'] for row in runner.blockers))
        self.assertEqual(evidence['status'], 'failed')

    def test_controller_terminates_unresponsive_child_after_failure_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)/'mixed-evidence.json'
            child_code = (
                'import json,sys,threading,time; '
                'open(sys.argv[1],"w").write(json.dumps({"status":"failed",'
                '"completed":False,"phases":[]})); '
                'threading.Thread(target=threading.Event().wait).start(); '
                'time.sleep(30)'
            )
            process = subprocess.Popen([sys.executable, '-c', child_code, str(evidence)],
                                       stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL)
            started = time.monotonic()
            try:
                exit_code, timed_out, stopped = CONTROLLER.wait_child(
                    process, evidence, timeout=3, poll_interval=.02,
                    terminate_grace=.3)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=1)
        self.assertLess(time.monotonic()-started, 2.0)
        self.assertNotEqual(exit_code, 0)
        self.assertFalse(timed_out)
        self.assertTrue(stopped)

    def test_controller_leaves_evidence_directory_for_child_and_validates_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private = root/'private'
            private.mkdir()
            source = root/'source.py'
            source.write_text('# offline controller fixture\n')
            output = private/'controller-success-mqtt-http-mixed'
            evidence = {
                'status': 'complete', 'completed': True, 'version': VERSION,
                'blockers': [], 'http_prechecks': 'passed', 'http_postchecks': 'passed',
                'phases': [{**row, 'completed': True} for row in CONTROLLER.PLAN],
            }
            child_code = (
                'from pathlib import Path; import json,sys; '
                'p=Path(sys.argv[1]); assert not p.exists(); p.mkdir(mode=0o700); '
                'Path(p,"mixed-evidence.json").write_text(sys.argv[2]); '
            )
            command = [sys.executable, '-c', child_code, str(output), json.dumps(evidence)]
            with mock.patch.object(CONTROLLER, 'PRIVATE', private), \
                    mock.patch.object(CONTROLLER, 'SOURCES',
                                      {name: source for name in CONTROLLER.SOURCES}), \
                    mock.patch.object(CONTROLLER, 'build_command', return_value=command), \
                    mock.patch.object(CONTROLLER.importlib.util, 'find_spec', return_value=object()):
                result = CONTROLLER.run('controller-success', VERSION, execute=True)
        self.assertEqual(result['status'], 'complete')
        self.assertTrue(result['completed'])
        self.assertEqual(result['process']['exit_code'], 0)
        self.assertTrue(result['source_unchanged'])
        self.assertTrue(result['source_copies_unchanged'])

    def test_controller_archival_failure_is_recorded_without_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            private = Path(directory)/'private'
            private.mkdir()
            with mock.patch.object(CONTROLLER, 'PRIVATE', private), \
                    mock.patch.object(CONTROLLER.importlib.util, 'find_spec', return_value=object()), \
                    mock.patch.object(CONTROLLER, 'archive_sources',
                                      side_effect=OSError('offline archive failure')):
                with self.assertRaisesRegex(OSError, 'offline archive failure'):
                    CONTROLLER.run('archive-failure', VERSION, execute=True)
            controller_file = private/'archive-failure-mqtt-http-mixed-controller'/'controller.json'
            record = json.loads(controller_file.read_text())
        self.assertEqual(record['status'], 'failed')
        self.assertFalse(record['completed'])
        self.assertIsNone(record['source_manifest'])
        self.assertTrue(any('offline archive failure' in item for item in record['blockers']))

    def test_tracked_controller_import_needs_no_ignored_build_script(self):
        expected = Path(__file__).resolve().parent/'device/mqtt_mixed_controller.py'
        self.assertEqual(Path(CONTROLLER.__file__).resolve(), expected)
        completed = subprocess.run(
            [sys.executable, '-S', '-c',
             'from tests.device import mqtt_mixed_controller as c; print(c.__file__)'],
            cwd=CONTROLLER.ROOT, check=True, capture_output=True, text=True, timeout=5)
        self.assertEqual(Path(completed.stdout.strip()).resolve(), expected)


if __name__ == '__main__':
    unittest.main()
