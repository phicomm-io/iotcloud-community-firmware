#!/usr/bin/env python3
"""Bounded HTTP/MQTT read-only mixed-load runner for the three shared candidates.

All broker publications are QoS 1, non-retained status.get requests. Dry-run is
the default. Device failure injection and broker disconnects belong in host tests.
"""
import argparse
import base64
from collections import Counter
from contextlib import contextmanager
import copy
import fcntl
import importlib.util
import json
import math
from pathlib import Path
import queue
import statistics
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/status101'
PRIVATE = BASE / 'private'
LOCK_PATH = Path('/tmp/phicomm-readonly-stress.lock')
MQTT_CANDIDATE = BASE / 'readonly_mqtt_candidate.py'
TARGETS = {
    '10.117.152.111': ('b0f893303134', 'a1'),
    '10.117.152.102': ('D0BAE463E12C', 'tc1'),
    '10.117.152.110': ('b0f8932864b6', 'm1'),
}
SHORT_PLAN = (
    {'name': 'mixed-1', 'http_clients': 1, 'mqtt_window': 1, 'seconds': 60},
    {'name': 'mixed-4', 'http_clients': 4, 'mqtt_window': 2, 'seconds': 120},
    {'name': 'mixed-8', 'http_clients': 8, 'mqtt_window': 4, 'seconds': 60},
    {'name': 'mixed-16', 'http_clients': 16, 'mqtt_window': 4, 'seconds': 60},
)
SOAK_PLAN = ({'name': 'mixed-soak', 'http_clients': 4, 'mqtt_window': 4,
              'seconds': 14400, 'mqtt_period': 1.0},)
HTTP_PERIOD = 1.0
MQTT_RESULT_TIMEOUT = 12.0
MQTT_PUBACK_TIMEOUT = 5.0
HTTP_REQUEST_TIMEOUT = 5.0
PHASE_SHUTDOWN_GRACE = 2.0
MQTT_NO_SUCCESS_LIMIT = 30.0
HTTP_RESPONSE_GAP_LIMIT = 30.0
NETWORK_NOT_READY_LIMIT = 30.0
RECOVERY_BUSY_MAX_RETRIES = 3
RECOVERY_BUSY_RETRY_DELAY = 1.0
RECOVERY_BUSY_TOTAL_TIMEOUT = 15.0
RUN_TIMEOUT = 15000.0
OUTCOME_NAMES = ('success', 'busy', 'timeout', 'transport', 'other', 'cancelled')
QUEUE_FIELDS = (
    'mqtt_pending', 'mqtt_peak', 'mqtt_overflow', 'mqtt_max_delay_ms',
    'mqtt_rx_queued', 'mqtt_tx_queued', 'mqtt_inflight', 'mqtt_queue_bytes',
    'mqtt_peak_bytes', 'mqtt_credit_bytes', 'mqtt_rejected', 'mqtt_owner_faults',
    'mqtt_generation', 'mqtt_rx_buffer_bytes', 'mqtt_tx_buffer_bytes',
    'mqtt_protocol_depth', 'mqtt_operation', 'mqtt_last_error_stage', 'mqtt_last_error',
    'mqtt_errors', 'mqtt_reconnects', 'mqtt_max_queue_delay_ms',
)
STACK_FIELDS = ('mqtt_stack_watermark', 'mqtt_stack_high_watermark', 'mqtt_stack_free')
MESSAGE_QUEUE_LIMIT = 512
PROGRESS_CHECKPOINT_INTERVAL = 60.0


def phase_deadlines(started, duration):
    load_deadline = started + duration
    drain_margin = (MQTT_RESULT_TIMEOUT + MQTT_PUBACK_TIMEOUT +
                    HTTP_REQUEST_TIMEOUT + PHASE_SHUTDOWN_GRACE)
    return load_deadline, load_deadline + drain_margin


def validate_targets(specs):
    parsed = {}
    for spec in specs:
        if '=' not in spec:
            raise ValueError('Targets must use IP=expected_device_id')
        ip, device_id = spec.split('=', 1)
        if ip in parsed:
            raise ValueError('Duplicate target IP')
        parsed[ip] = device_id
    expected = {ip: ident for ip, (ident, _model) in TARGETS.items()}
    if (not parsed or not set(parsed) <= set(expected) or
            any(parsed[ip].lower() != expected[ip].lower() for ip in parsed)):
        raise ValueError('Explicit targets must be an exact nonempty authorized subset')
    if '10.117.152.110' in parsed and parsed != expected:
        raise ValueError('M1 may only be selected in the legacy complete three-device scope')
    return {ip: parsed[ip] for ip in TARGETS if ip in parsed}


def validate_version(version):
    if not isinstance(version, str) or not version.startswith('0.9.15-') or len(version) > 72:
        raise ValueError('Version must use the 0.9.15-<candidate> form')
    suffix = version[len('0.9.15-'):]
    if not suffix or not all(ch.isalnum() or ch in '.-' for ch in suffix):
        raise ValueError('Version contains unsupported characters')
    return version


def make_status_payload(request_id):
    if not isinstance(request_id, str) or not request_id.startswith('readonly-mixed-'):
        raise ValueError('Invalid read-only request ID')
    return {'request_id': request_id, 'action': 'status.get', 'params': {}}


def guard_status_payload(payload):
    if (not isinstance(payload, dict) or set(payload) != {'request_id', 'action', 'params'} or
            payload.get('action') != 'status.get' or payload.get('params') != {} or
            not isinstance(payload.get('request_id'), str) or
            not payload['request_id'].startswith('readonly-mixed-')):
        raise ValueError('Only unique read-only status.get requests are permitted')
    return payload


class BoundedWindow:
    """Tracks sent requests without allowing an unbounded backlog."""
    def __init__(self, maximum):
        if type(maximum) is not int or not 1 <= maximum <= 4:
            raise ValueError('Outstanding MQTT window must be between one and four')
        self.maximum = maximum
        self.pending = {}

    def add(self, request_id, started):
        if len(self.pending) >= self.maximum:
            raise RuntimeError('MQTT outstanding window is full')
        if request_id in self.pending:
            raise ValueError('MQTT request ID was reused')
        self.pending[request_id] = started

    def finish(self, request_id):
        return self.pending.pop(request_id, None)

    def expire(self, now, timeout):
        expired = []
        for request_id, started in list(self.pending.items()):
            if now - started >= timeout:
                expired.append((request_id, now-started))
                del self.pending[request_id]
        return expired


def classify_mqtt_message(topic, body, retained, request_id, result_topic, state_topic):
    if topic == state_topic:
        return 'retained_telemetry' if retained else 'fresh_telemetry'
    if topic != result_topic:
        return 'other_topic'
    if not isinstance(body, dict) or body.get('request_id') != request_id:
        return 'unmatched_result'
    if retained:
        return 'retained_result'
    code = body.get('code')
    if code == 'ok' and isinstance(body.get('data'), dict):
        return 'success'
    if code == 'busy':
        return 'busy'
    return 'other'


def summarize_latencies(rows):
    result = {}
    for category in OUTCOME_NAMES:
        values = sorted(row['elapsed_ms'] for row in rows
                        if row.get('outcome') == category and
                        type(row.get('elapsed_ms')) in (int, float) and
                        math.isfinite(row['elapsed_ms']))
        result[category] = {
            'count': sum(row.get('outcome') == category for row in rows),
            'latency_samples': len(values),
            'p50_ms': statistics.median(values) if values else None,
            'p95_ms': values[math.ceil(len(values)*.95)-1] if values else None,
            'p99_ms': values[math.ceil(len(values)*.99)-1] if values else None,
        }
    return result


def optional_diagnostics(memory_rows):
    successful = [row.get('data', {}) for row in memory_rows if row.get('ok') is True]
    result = {}
    for name, key in [('queue.'+field, field) for field in QUEUE_FIELDS] + [
            ('stack.'+field, field) for field in STACK_FIELDS]:
        values = [sample.get(key) for sample in successful if key in sample]
        if not values:
            result[name] = {'availability': 'NOT_AVAILABLE', 'samples': 0}
        elif any(type(value) is not int for value in values):
            result[name] = {'availability': 'INVALID_TYPE', 'samples': len(values)}
        else:
            result[name] = {'availability': 'available', 'samples': len(values),
                            'min': min(values), 'max': max(values), 'last': values[-1]}
    return result


def phase_plan(profile):
    if profile == 'short':
        return [dict(row) for row in SHORT_PLAN]
    if profile == 'soak':
        return [dict(row) for row in SOAK_PLAN]
    raise ValueError('Unknown profile')


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError('Could not load required read-only helper')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_runtime():
    sys.path.insert(0, str(ROOT))
    from tests.device.readonly_stress import Runner
    mqtt_helpers = load_module(MQTT_CANDIDATE, 'readonly_mqtt_candidate_for_mixed')
    MQTTClient, mqtt_root, mqtt_client_id = mqtt_helpers.load_primary_mqtt_helpers()
    return Runner, mqtt_helpers, MQTTClient, mqtt_root, mqtt_client_id


@contextmanager
def run_lock():
    with LOCK_PATH.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


class Evidence:
    def __init__(self, path, label, version, profile, plan, targets=TARGETS):
        self.path = path
        self.data = {
            'schema': 1, 'label': label, 'version': version, 'profile': profile,
            'status': 'running', 'completed': False, 'started_at': time.time(),
            'selected_targets': list(targets),
            'targets': {ip: {'device_id': TARGETS[ip][0], 'model': TARGETS[ip][1]}
                        for ip in targets},
            'device_status': {ip: ('SELECTED' if ip in targets else 'PAUSED')
                              for ip in TARGETS},
            'plan': plan, 'phases': [], 'http': [], 'mqtt': [], 'telemetry': [],
            'recovery': [], 'recovery_attempts': [], 'network_confirmations': [],
            'protocol_errors': [], 'unmatched_mqtt': [],
            'queue_drops': [], 'queue_overflow_events': [], 'blockers': [],
        }
        self.lock = threading.RLock()
        self.save()

    def save(self):
        with self.lock:
            snapshot = copy.deepcopy(self.data)
        encoded = json.dumps(snapshot, indent=2, sort_keys=True) + '\n'
        temp = self.path.with_suffix('.tmp')
        temp.write_text(encoded)
        temp.chmod(0o600)
        temp.replace(self.path)
        self.path.chmod(0o600)

    def append(self, collection, row):
        with self.lock:
            self.data[collection].append(row)


class BoundedMessageQueue(queue.Queue):
    """Keep the MQTT callback queue bounded and count dropped old messages."""
    def __init__(self, maximum=MESSAGE_QUEUE_LIMIT, on_drop=None):
        super().__init__(maxsize=maximum)
        self.dropped = 0
        self.on_drop = on_drop

    def put(self, item, block=True, timeout=None):
        del block, timeout
        dropped = False
        with self.mutex:
            if self._qsize() >= self.maxsize:
                self._get()
                self.dropped += 1
                self.unfinished_tasks -= 1
                self.not_full.notify()
                dropped = True
            self._put(item)
            self.unfinished_tasks += 1
            self.not_empty.notify()
        if dropped and self.on_drop is not None:
            self.on_drop()


class MQTTProtocolError(RuntimeError):
    pass


def _validate_device_data(data, ip, initial, version):
    if not isinstance(data, dict):
        raise ValueError('MQTT status data was not an object')
    ident, model = TARGETS[ip]
    reported_id = data.get('device_id', data.get('id'))
    if not isinstance(reported_id, str) or reported_id.lower() != ident.lower():
        raise ValueError('MQTT status identity mismatch')
    if data.get('model') != model or data.get('version') != version:
        raise ValueError('MQTT status model/version mismatch')
    controls = {
        'a1': ('requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled'),
        'tc1': ('socket_states', 'child_lock', 'led_enabled'),
        'm1': ('brightness_requested',),
    }[model]
    for key in controls:
        if key not in initial or key not in data or data[key] != initial[key]:
            raise ValueError('MQTT status control mismatch: '+key)
    network = _network_fields(data)
    uptime, base_uptime = data.get('uptime_seconds'), initial.get('uptime_seconds')
    if (type(uptime) not in (int, float) or not math.isfinite(uptime) or
            type(base_uptime) not in (int, float) or uptime < base_uptime):
        raise ValueError('MQTT status uptime missing or below baseline')
    if data.get('watchdog_state') not in (None, 'healthy', 'starting'):
        raise ValueError('MQTT status watchdog is unhealthy')
    return {'device_id': reported_id, 'model': model, 'version': version,
            'uptime_seconds': uptime, **{key: data[key] for key in controls}, **network}


def _network_fields(data):
    result = {}
    for key in ('wifi_connected', 'mqtt_connected'):
        if type(data.get(key)) is not bool:
            raise ValueError('Status network field must be boolean: '+key)
        result[key] = data[key]
    return result


class NetworkReadiness:
    """Run-wide, sticky network degradation deadline; never sends traffic."""

    def __init__(self, runner, evidence, monotonic=time.monotonic, targets=TARGETS):
        self.runner, self.evidence, self.monotonic = runner, evidence, monotonic
        self.lock = threading.Lock()
        self.state = {ip: {'degraded_since_monotonic': None,
                           'last_not_ready_observed_monotonic': None,
                           'maximum_not_ready_seconds': 0.0, 'expired': False}
                      for ip in targets}
        self.evidence.data.setdefault('network_events', [])
        self.evidence.data.setdefault('network_readiness', {})

    def _update_locked(self, now, phase):
        blockers = []
        for ip, state in self.state.items():
            since = state['degraded_since_monotonic']
            elapsed = 0.0 if since is None else max(0.0, now-since)
            state['current_not_ready_seconds'] = elapsed
            state['maximum_not_ready_seconds'] = max(state['maximum_not_ready_seconds'], elapsed)
            state['limit_seconds'] = NETWORK_NOT_READY_LIMIT
            if elapsed >= NETWORK_NOT_READY_LIMIT and not state['expired']:
                state['expired'] = True
                reason = 'Continuous network not-ready observation exceeded %.1fs for %s' % (NETWORK_NOT_READY_LIMIT, ip)
                state['stop_reason'] = reason
                blockers.append((ip, reason))
                self.evidence.append('network_events', {
                    'ip': ip, 'phase': phase, 'event': 'network-deadline-exceeded',
                    'observed_monotonic': now, 'not_ready_seconds': elapsed,
                    'reason': reason})
        with self.evidence.lock:
            self.evidence.data['network_readiness'] = copy.deepcopy(self.state)
        return blockers

    def _block(self, blockers):
        # Never invoke Runner.block while holding the observer lock.
        for ip, reason in blockers:
            self.runner.block(ip, reason)

    def check(self, phase='network-monitor', now=None):
        now = self.monotonic() if now is None else now
        with self.lock:
            blockers = self._update_locked(now, phase)
        self._block(blockers)

    def require_ready(self, phase, ip=None):
        self.check(phase)
        with self.lock:
            blockers = [(target, 'Unresolved network degradation at '+phase)
                        for target, state in self.state.items()
                        if (ip is None or target == ip) and
                        state['degraded_since_monotonic'] is not None]
        self._block(blockers)

    def observe(self, ip, data, phase, source, started=None, *, can_restore=False, now=None):
        network = _network_fields(data)
        now = self.monotonic() if now is None else now
        ready = all(network.values())
        with self.lock:
            # Check before accepting true: late recovery cannot erase an overrun.
            blockers = self._update_locked(now, phase)
            state = self.state[ip]
            since = state['degraded_since_monotonic']
            state.update(network)
            state.update(last_observation_source=source, last_observed_monotonic=now,
                         last_request_started_monotonic=started,
                         last_observation_can_restore=can_restore)
            if not ready:
                state['last_not_ready_observed_monotonic'] = now
                if since is None:
                    state['degraded_since_monotonic'] = now
                    self.evidence.append('network_events', {
                        'ip': ip, 'phase': phase, 'event': 'network-degraded',
                        'source': source, 'observed_monotonic': now, **network})
            elif (ready and since is not None and not state['expired'] and
                  can_restore and started is not None and
                  started >= state['last_not_ready_observed_monotonic']):
                self.evidence.append('network_events', {
                    'ip': ip, 'phase': phase, 'event': 'network-ready',
                    'source': source, 'observed_monotonic': now,
                    'not_ready_seconds': now-since, **network})
                state['degraded_since_monotonic'] = None
            blockers += self._update_locked(now, phase)
        self._block(blockers)


def _observe_network(runner, data, ip, phase, source, started=None, *, can_restore=False):
    monitor = getattr(runner, 'mixed_network_readiness', None)
    if monitor is not None:
        monitor.observe(ip, data, phase, source, started, can_restore=can_restore)


def _check_network(runner, phase):
    monitor = getattr(runner, 'mixed_network_readiness', None)
    if monitor is not None:
        monitor.check(phase)


def _network_observed_runner(base):
    class ObservedRunner(base):
        def sample(self, ip, phase, *args, **kwargs):
            _check_network(self, phase)
            row = super().sample(ip, phase, *args, **kwargs)
            if row.get('ok') is True and row.get('action') == 'status.get':
                try:
                    _network_fields(row)
                    if not self.stop.is_set():
                        _observe_network(self, row, ip, phase, 'http-status',
                                         row.get('request_started_monotonic'), can_restore=True)
                except ValueError as exc:
                    self.block(ip, 'Invalid HTTP status network fields: '+str(exc))
            _check_network(self, phase)
            return row
    return ObservedRunner


class DeviceMQTTWorker:
    def __init__(self, ip, client, root, runner, evidence, stop):
        self.ip, self.client, self.root = ip, client, root
        self.runner, self.evidence, self.stop = runner, evidence, stop
        self.failure = None
        self.phase_abort_reason = None
        self.phase_started = time.monotonic()
        self.phase_deadline = None
        self.last_success = self.phase_started
        self.last_correlated_response = self.phase_started
        self.max_success_gap_seconds = 0.0
        self.pending = None
        self.result_topic = root + '/result'
        self.state_topic = root + '/state'

    def publish_qos1(self, topic, payload, timeout):
        payload = guard_status_payload(payload)
        info = self.client.client.publish(topic, json.dumps(
            payload, separators=(',', ':'), allow_nan=False).encode(), qos=1, retain=False)
        if info.rc != 0:
            raise ConnectionError('MQTT QoS1 publish failed')
        info.wait_for_publish(timeout=timeout)
        if not info.is_published():
            raise TimeoutError('MQTT PUBACK deadline expired')

    def record_outcome(self, phase, outcome, started, now, request_id, code=None,
                       identity=None):
        row = {
            'ip': self.ip, 'phase': phase, 'request_id': request_id,
            'action': 'status.get', 'qos': 1, 'retain': False,
            'outcome': outcome, 'code': code,
            'elapsed_ms': max(0.0, (now-started)*1000),
        }
        if identity:
            row.update(identity)
        self.evidence.append('mqtt', row)

    def run_window(self, phase, deadline, maximum, period):
        pending = BoundedWindow(maximum)
        self.pending = pending
        self.phase_started = time.monotonic()
        self.phase_deadline = deadline
        self.last_success = self.phase_started
        self.last_correlated_response = self.phase_started
        self.max_success_gap_seconds = 0.0
        next_send = time.monotonic()
        sending_done = False
        while (not sending_done or pending.pending) and not self.stop.is_set() and not self.runner.stop.is_set():
            _check_network(self.runner, phase)
            if self.runner.stop.is_set():
                break
            now = time.monotonic()
            if now-self.last_correlated_response >= MQTT_NO_SUCCESS_LIMIT:
                self.phase_abort_reason = 'No correlated MQTT response for the bounded liveness interval'
                self.stop.set()
                break
            if not self.client.connected.is_set():
                for request_id, started in list(pending.pending.items()):
                    self.record_outcome(phase, 'transport', started, now, request_id)
                    pending.finish(request_id)
                self.phase_abort_reason = 'MQTT connection was lost during mixed phase'
                self.stop.set()
                break
            if now >= deadline:
                sending_done = True
            if not sending_done and len(pending.pending) < maximum and now >= next_send:
                request_id = 'readonly-mixed-' + uuid.uuid4().hex
                payload = make_status_payload(request_id)
                started = time.monotonic()
                pending.add(request_id, started)
                try:
                    self.publish_qos1(self.root+'/command', payload,
                                      min(MQTT_PUBACK_TIMEOUT, max(.05, deadline-started)))
                except TimeoutError:
                    sent = pending.finish(request_id)
                    self.record_outcome(phase, 'timeout', sent or started, time.monotonic(), request_id)
                    next_send = max(next_send + period, time.monotonic())
                except OSError as exc:
                    self.record_outcome(phase, 'transport', pending.finish(request_id) or started,
                                        time.monotonic(), request_id)
                    self.phase_abort_reason = 'MQTT publish transport failed: '+type(exc).__name__
                    self.stop.set()
                    break
                except Exception as exc:
                    self.record_outcome(phase, 'transport', pending.finish(request_id) or started,
                                        time.monotonic(), request_id)
                    self.phase_abort_reason = 'MQTT publish failed: '+type(exc).__name__
                    self.stop.set()
                    break
                next_send = max(next_send + period, time.monotonic())
            try:
                topic, body, retained, _raw = self.client.messages.get(timeout=.05)
            except queue.Empty:
                topic = body = None
                retained = False
            else:
                self.client.messages.task_done()
            now = time.monotonic()
            if topic is not None:
                if topic == self.state_topic:
                    if retained:
                        self.evidence.append('telemetry', {
                            'ip': self.ip, 'phase': phase, 'retained': True,
                            'collection_stage': 'load', 'fresh': False,
                            'reason': 'initial retained snapshot excluded',
                            'phase_started_monotonic': self.phase_started,
                            'phase_deadline_monotonic': self.phase_deadline,
                        })
                    elif isinstance(body, dict):
                        try:
                            data = body.get('data', body)
                            identity = _validate_device_data(
                                data, self.ip, self.runner.before[self.ip]['state'], self.runner.version)
                            _observe_network(self.runner, data, self.ip, phase, 'mqtt-telemetry')
                            self.evidence.append('telemetry', {
                                'ip': self.ip, 'phase': phase, 'retained': False,
                                'fresh': True, 'validated': True,
                                'collection_stage': 'load',
                                'consumer_observed_monotonic': now,
                                'phase_started_monotonic': self.phase_started,
                                'phase_deadline_monotonic': self.phase_deadline,
                                'observed_within_phase_window': now <= self.phase_deadline,
                                'freshness_basis': 'post-subscribe non-retained delivery; source sample time unavailable',
                                'timestamp_semantics': 'host queue dequeue time, not broker ingress time',
                                'uptime_source_seconds': identity['uptime_seconds'],
                                'uptime_reset_check': 'not_evaluated_without_source_timestamp',
                                **identity,
                            })
                        except ValueError as exc:
                            self.failure = 'Invalid fresh telemetry: '+str(exc)
                            self.runner.block(self.ip, self.failure)
                            break
                    else:
                        self.evidence.append('telemetry', {
                            'ip': self.ip, 'phase': phase, 'retained': False,
                            'collection_stage': 'load', 'fresh': False,
                            'reason': 'protocol error: non-object state payload',
                            'phase_started_monotonic': self.phase_started,
                            'phase_deadline_monotonic': self.phase_deadline,
                        })
                        self.evidence.append('protocol_errors', {
                            'ip': self.ip, 'phase': phase, 'topic_kind': 'state',
                            'error': 'non_object_payload',
                        })
                        self.failure = 'Protocol error on MQTT state topic'
                        self.runner.block(self.ip, self.failure)
                        break
                elif topic == self.result_topic and not isinstance(body, dict):
                    self.failure = 'Protocol error: matching MQTT result topic was not a JSON object'
                    self.evidence.append('protocol_errors', {
                        'ip': self.ip, 'phase': phase, 'topic_kind': 'result',
                        'error': 'non_object_payload',
                    })
                    self.runner.block(self.ip, self.failure)
                    break
                elif topic == self.result_topic and isinstance(body, dict):
                    request_id = body.get('request_id')
                    started = pending.finish(request_id) if isinstance(request_id, str) else None
                    if started is None:
                        self.evidence.append('unmatched_mqtt', {
                            'ip': self.ip, 'phase': phase, 'topic_kind': 'result',
                            'retained': bool(retained),
                        })
                    else:
                        category = classify_mqtt_message(topic, body, retained, request_id,
                                                         self.result_topic, self.state_topic)
                        code = body.get('code')
                        if category == 'success':
                            try:
                                identity = _validate_device_data(
                                    body['data'], self.ip, self.runner.before[self.ip]['state'],
                                    self.runner.version)
                                checked = dict(body['data'])
                                checked.setdefault('device_id', identity['device_id'])
                                self.runner.check(self.ip, checked, started, now)
                                _observe_network(self.runner, checked, self.ip, phase,
                                                 'mqtt-correlated', started, can_restore=True)
                                self.max_success_gap_seconds = max(
                                    self.max_success_gap_seconds, now-self.last_success)
                                self.last_success = now
                                self.last_correlated_response = now
                                self.record_outcome(phase, 'success', started, now, request_id,
                                                    code, identity)
                            except ValueError as exc:
                                self.failure = 'Invalid correlated MQTT result: '+str(exc)
                                self.runner.block(self.ip, self.failure)
                                break
                        elif category == 'busy' and not retained:
                            self.last_correlated_response = now
                            self.record_outcome(phase, 'busy', started, now, request_id, code)
                        else:
                            self.record_outcome(phase, 'other', started, now, request_id, code)
                            self.failure = 'Unexpected correlated MQTT result'
                            self.runner.block(self.ip, self.failure)
                            break
            for request_id, elapsed in pending.expire(now, MQTT_RESULT_TIMEOUT):
                self.record_outcome(phase, 'timeout', now-elapsed, now, request_id)
            if now-self.last_correlated_response >= MQTT_NO_SUCCESS_LIMIT:
                self.phase_abort_reason = 'No correlated MQTT response for the bounded liveness interval'
                self.stop.set()
                break
            self.stop.wait(min(.02, max(0.0, deadline-time.monotonic())))
        if pending.pending:
            now = time.monotonic()
            for request_id, started in list(pending.pending.items()):
                outcome = 'cancelled'
                self.record_outcome(phase, outcome, started, now, request_id,
                                    code='phase_stopped')
                pending.finish(request_id)

    def run_guarded(self, phase, deadline, maximum, period):
        try:
            self.run_window(phase, deadline, maximum, period)
        except BaseException as exc:
            now = time.monotonic()
            if self.pending is not None:
                for request_id, started in list(self.pending.pending.items()):
                    self.record_outcome(phase, 'transport', started, now, request_id)
                    self.pending.finish(request_id)
            self.failure = 'Unhandled MQTT worker exception: '+type(exc).__name__
            self.runner.block(self.ip, self.failure)
            self.stop.set()


def _http_worker(runner, ip, phase, deadline, number, stop, period):
    n = number
    while time.monotonic() < deadline and not stop.is_set() and not runner.stop.is_set():
        params = {'view': 'live'} if n % 2 == 0 else {}
        sample_started = time.monotonic()
        row = runner.sample(ip, phase, params=params)
        row.setdefault('observed_elapsed_ms', max(0.0, (time.monotonic()-sample_started)*1000))
        elapsed = row.get('ms', row.get('observed_elapsed_ms', 0))
        stop.wait(max(0.0, period - elapsed/1000.0))
        n += 1


def _classify_http(row):
    if row.get('ok') is True:
        return 'success'
    if row.get('http') == 503 or row.get('code') == 'busy':
        return 'busy'
    error = str(row.get('error', ''))
    if 'Timeout' in error or 'timeout' in error.lower():
        return 'timeout'
    if 'http' not in row or isinstance(row.get('error'), str):
        return 'transport'
    return 'other'


class HTTPResponseGap:
    """Soak availability from completed HTTP records; no additional requests."""

    def __init__(self, targets, started):
        self.state = {ip: {kind: started for kind in ('response', 'success')}
                      for ip in targets}
        self.maximum = {ip: dict(response=0.0, success=0.0) for ip in targets}

    def observe(self, rows):
        for row in rows:
            ip, ended = row.get('ip'), row.get('request_ended_monotonic')
            if ip not in self.state or ended is None:
                continue
            success = row.get('ok') is True and row.get('http') == 200 and row.get('code') == 'ok'
            busy = row.get('http') in (200, 503) and row.get('code') == 'busy' and not row.get('error')
            for kind, valid in (('response', success or busy), ('success', success)):
                if valid:
                    self.maximum[ip][kind] = max(self.maximum[ip][kind], ended-self.state[ip][kind])
                    self.state[ip][kind] = max(self.state[ip][kind], ended)

    def snapshot(self, now):
        result = {}
        for ip, state in self.state.items():
            result[ip] = {'response_gap_limit_seconds': HTTP_RESPONSE_GAP_LIMIT}
            for kind, last in state.items():
                gap = max(0.0, now-last)
                self.maximum[ip][kind] = max(self.maximum[ip][kind], gap)
                result[ip]['last_'+kind+'_gap_seconds'] = gap
                result[ip]['maximum_'+kind+'_gap_seconds'] = self.maximum[ip][kind]
        return result


def run_phase(runner, clients, roots, evidence, phase_plan, profile, targets=TARGETS):
    plan = phase_plan
    phase = plan['name']
    started = time.monotonic()
    deadline, hard_deadline = phase_deadlines(started, plan['seconds'])
    http_before = len(runner.rows)
    http_cursor = http_before
    http_gap = HTTPResponseGap(targets, started) if profile == 'soak' else None
    dropped_before = {ip: getattr(clients[ip].messages, 'dropped', 0) for ip in targets}
    phase_row = {**plan, 'started_at': time.time(), 'completed': False}
    phase_row['progress_checkpoints'] = []
    evidence.data['phases'].append(phase_row)
    mqtt_workers = []
    from concurrent.futures import ThreadPoolExecutor
    stop = threading.Event()
    pool = ThreadPoolExecutor(max_workers=plan['http_clients']*len(targets))
    futures = []
    mqtt_threads = []
    hard_timed_out = False

    def check_http_gap():
        nonlocal http_cursor
        if http_gap is None:
            return
        # Copy only under the lock. Blocking, persistence and all network I/O
        # remain outside it, as in Runner.sample().
        with runner.lock:
            rows = list(runner.rows[http_cursor:])
            http_cursor = len(runner.rows)
        http_gap.observe(row for row in rows if row.get('phase') == phase)
        snapshot = http_gap.snapshot(time.monotonic())
        phase_row['http_response_liveness'] = snapshot
        for ip, row in snapshot.items():
            if row['maximum_response_gap_seconds'] >= HTTP_RESPONSE_GAP_LIMIT:
                reason = 'HTTP response gap exceeded %.1fs in %s for %s' % (HTTP_RESPONSE_GAP_LIMIT, phase, ip)
                row['stop_reason'] = reason
                phase_row.setdefault('abort_reason', reason)
                if not runner.stop.is_set():
                    runner.block(ip, reason)
                stop.set()

    try:
        futures = [pool.submit(_http_worker, runner, ip, phase, deadline, n, stop, HTTP_PERIOD)
                   for ip in targets for n in range(plan['http_clients'])]
        queue_overflow_lock = threading.Lock()
        queue_overflow_seen = set()
        for ip in targets:
            def on_drop(target_ip=ip):
                with queue_overflow_lock:
                    first_drop = target_ip not in queue_overflow_seen
                    queue_overflow_seen.add(target_ip)
                if first_drop:
                    evidence.append('queue_overflow_events', {
                        'ip': target_ip, 'phase': phase, 'dropped_oldest': True,
                    })
                    runner.block(target_ip, 'Bounded host MQTT message queue overflowed in '+phase)
                    stop.set()

            clients[ip].messages.on_drop = on_drop
            worker = DeviceMQTTWorker(ip, clients[ip], roots[ip], runner, evidence, stop)
            mqtt_workers.append(worker)
            period = plan.get('mqtt_period', 0.0)
            thread = threading.Thread(target=worker.run_guarded,
                                      args=(phase, deadline, plan['mqtt_window'], period),
                                      name='mqtt-'+ip.rsplit('.', 1)[1], daemon=True)
            mqtt_threads.append(thread)
            thread.start()
        next_memory = time.monotonic()+15
        next_checkpoint = time.monotonic()+PROGRESS_CHECKPOINT_INTERVAL
        while any(not future.done() for future in futures) or any(t.is_alive() for t in mqtt_threads):
            check_http_gap()
            _check_network(runner, phase)
            if time.monotonic() >= hard_deadline:
                hard_timed_out = True
                stop.set()
                runner.block('', 'Mixed phase exceeded hard deadline: '+phase)
                phase_row['hard_deadline_exceeded'] = True
                phase_row['elapsed_seconds'] = time.monotonic()-started
                phase_row['progress_checkpoints'].append({
                    'elapsed_seconds': phase_row['elapsed_seconds'],
                    'http_rows': len(runner.rows)-http_before,
                    'mqtt_rows': sum(row.get('phase') == phase for row in evidence.data['mqtt']),
                    'completed': False,
                    'hard_deadline_exceeded': True,
                })
                runner.save()
                evidence.save()
                print(json.dumps({'event': 'mixed-hard-deadline', 'phase': phase,
                                  'elapsed_seconds': phase_row['elapsed_seconds']},
                                 sort_keys=True), flush=True)
                break
            if runner.stop.is_set() or any(worker.failure for worker in mqtt_workers):
                stop.set()
            if time.monotonic() >= next_memory and not runner.stop.is_set():
                for ip in targets:
                    runner.memory_sample(ip, phase)
                    check_http_gap()
                    if http_gap is not None and runner.stop.is_set():
                        break
                next_memory = time.monotonic()+15
            if time.monotonic() >= next_checkpoint:
                checkpoint = {
                    'elapsed_seconds': time.monotonic()-started,
                    'http_rows': len(runner.rows)-http_before,
                    'mqtt_rows': sum(row.get('phase') == phase for row in evidence.data['mqtt']),
                    'completed': False,
                }
                phase_row['progress_checkpoints'].append(checkpoint)
                runner.save()
                evidence.save()
                print(json.dumps({'event': 'mixed-progress', 'phase': phase, **checkpoint},
                                 sort_keys=True), flush=True)
                next_checkpoint = time.monotonic()+PROGRESS_CHECKPOINT_INTERVAL
            time.sleep(.05)
        if not hard_timed_out:
            for thread in mqtt_threads:
                thread.join(timeout=.1)
            if any(thread.is_alive() for thread in mqtt_threads):
                stop.set()
                runner.block('', 'MQTT phase worker remained alive after phase deadline')
            for future in futures:
                if future.done():
                    try:
                        future.result()
                    except BaseException as exc:
                        runner.block('', 'HTTP phase worker failed: '+type(exc).__name__)
                        stop.set()
        if hard_timed_out or any(t.is_alive() for t in mqtt_threads) or any(not f.done() for f in futures):
            pool.shutdown(wait=False, cancel_futures=True)
        else:
            pool.shutdown(wait=True)
    except BaseException:
        stop.set()
        pool.shutdown(wait=False, cancel_futures=True)
        raise
    check_http_gap()
    _check_network(runner, phase+'-end')
    for worker in mqtt_workers:
        if worker.failure:
            runner.block('', worker.failure)
    dropped = {ip: getattr(clients[ip].messages, 'dropped', 0)-dropped_before[ip]
               for ip in targets}
    evidence.data.setdefault('queue_drops', []).append({'phase': phase, 'per_device': dropped})
    if any(count for count in dropped.values()):
        runner.block('', 'Bounded host MQTT message queue dropped messages in '+phase)
    for row in runner.rows[http_before:]:
        if row.get('phase') == phase:
            evidence.data['http'].append({
                'ip': row['ip'], 'phase': phase, 'action': row.get('action'),
                'outcome': _classify_http(row), 'http': row.get('http'),
                'code': row.get('code'),
                'elapsed_ms': row.get('ms', row.get('observed_elapsed_ms')),
            })
    elapsed = time.monotonic()-started
    phase_row.update(ended_at=time.time(), elapsed_seconds=elapsed,
                     completed=(elapsed >= plan['seconds'] and not runner.stop.is_set() and
                                not hard_timed_out))
    if not phase_row['completed']:
        phase_row.setdefault('abort_reason', next(
            (worker.phase_abort_reason for worker in mqtt_workers if worker.phase_abort_reason),
            'Mixed phase ended before its requested duration'))
    phase_http = [row for row in evidence.data['http'] if row.get('phase') == phase]
    phase_row['http_outcomes'] = summarize_latencies(phase_http)
    phase_row['mqtt_outcomes'] = summarize_latencies(
        [row for row in evidence.data['mqtt'] if row.get('phase') == phase])
    for ip in targets:
        if not any(row.get('ip') == ip and row.get('outcome') == 'success' for row in phase_http):
            phase_row.setdefault('abort_reason', 'No successful HTTP status response for '+ip)
        if not any(row.get('ip') == ip and row.get('outcome') == 'success'
                   for row in evidence.data['mqtt'] if row.get('phase') == phase):
            if not any(row.get('ip') == ip and row.get('outcome') == 'busy'
                       for row in evidence.data['mqtt'] if row.get('phase') == phase):
                phase_row.setdefault('abort_reason', 'No correlated MQTT status response for '+ip)
    phase_row['telemetry_observation'] = {
        ip: _telemetry_observation(evidence.data['telemetry'], ip, phase, 'load')
        for ip in targets
    }
    phase_row['mqtt_success_liveness'] = {
        worker.ip: {
            'last_success_gap_seconds': max(0.0, time.monotonic()-worker.last_success),
            'maximum_success_gap_seconds': worker.max_success_gap_seconds,
            'last_correlated_response_gap_seconds': max(
                0.0, time.monotonic()-worker.last_correlated_response),
        }
        for worker in mqtt_workers
    }
    if phase_row.get('abort_reason') and not runner.stop.is_set():
        phase_row['completed'] = False
    evidence.save()
    if runner.stop.is_set() and not phase_row.get('abort_reason'):
        phase_row['abort_reason'] = 'Mixed phase stopped by a runner blocker'
    return phase_row.get('abort_reason')


def _telemetry_observation(rows, ip, phase, stage):
    selected = [row for row in rows if row.get('ip') == ip and row.get('phase') == phase and
                row.get('collection_stage') == stage]
    validated = sum(row.get('validated') is True and row.get('retained') is False
                    for row in selected)
    retained = sum(row.get('retained') is True for row in selected)
    within_window = sum(row.get('validated') is True and row.get('retained') is False and
                        row.get('observed_within_phase_window') is True for row in selected)
    return {
        'availability': 'observed' if validated else 'NOT_AVAILABLE',
        'validated_nonretained_count': validated,
        'observed_within_phase_window_count': within_window,
        'observed_after_phase_window_count': validated-within_window,
        'retained_excluded_count': retained,
        'source_timestamp_available': False,
    }


def receive_correlated(client, request_id, result_topic, state_topic, timeout,
                       telemetry_events, ip, phase, runner, *,
                       collection_stage='recovery', window_started=None,
                       include_raw=False, absolute_deadline=None,
                       monotonic=time.monotonic):
    started = monotonic() if window_started is None else window_started
    deadline = started+timeout if absolute_deadline is None else absolute_deadline
    while monotonic() < deadline:
        _check_network(runner, phase)
        if runner.stop.is_set():
            raise RuntimeError('Network/state gate stopped recovery')
        if not client.connected.is_set():
            raise ConnectionError('MQTT disconnected during phase recovery')
        try:
            topic, body, retained, _raw = client.messages.get(
                timeout=min(.1, max(.01, deadline-monotonic())))
        except queue.Empty:
            continue
        try:
            if topic == state_topic:
                if retained:
                    telemetry_events.append({
                        'ip': ip, 'phase': phase, 'retained': True, 'fresh': False,
                        'collection_stage': collection_stage,
                        'phase_started_monotonic': started,
                        'phase_deadline_monotonic': deadline,
                        'validated': False, 'reason': 'retained snapshot excluded',
                    })
                else:
                    if not isinstance(body, dict):
                        raise MQTTProtocolError('Non-object MQTT state payload during recovery')
                    data = body.get('data', body)
                    try:
                        identity = _validate_device_data(
                            data, ip, runner.before[ip]['state'], runner.version)
                    except ValueError as exc:
                        raise MQTTProtocolError('Invalid MQTT state telemetry during recovery') from exc
                    _observe_network(runner, data, ip, phase, 'mqtt-telemetry')
                    observed = time.monotonic()
                    telemetry_events.append({
                        'ip': ip, 'phase': phase, 'retained': False, 'fresh': True,
                        'collection_stage': collection_stage,
                        'validated': True, 'consumer_observed_monotonic': observed,
                        'observed_within_phase_window': observed <= deadline,
                        'phase_started_monotonic': started,
                        'phase_deadline_monotonic': deadline,
                        'freshness_basis': 'post-subscribe non-retained delivery; source sample time unavailable',
                        'timestamp_semantics': 'host queue dequeue time, not broker ingress time',
                        'uptime_source_seconds': identity['uptime_seconds'],
                        'uptime_reset_check': 'not_evaluated_without_source_timestamp',
                        **identity,
                    })
                continue
            if topic == result_topic:
                if not isinstance(body, dict):
                    raise MQTTProtocolError('Non-object MQTT result payload during recovery')
                if body.get('request_id') == request_id:
                    if include_raw:
                        return topic, body, retained, _raw
                    return topic, body, retained
        finally:
            client.messages.task_done()
    raise TimeoutError('Fresh correlated MQTT recovery result timed out')


def recover(runner, clients, roots, evidence, name, *, monotonic=time.monotonic,
            sleep=time.sleep, targets=TARGETS):
    _check_network(runner, name+'-start')
    runner.recover(name+'-http', seconds=45)
    _check_network(runner, name+'-http-end')
    if runner.stop.is_set():
        raise RuntimeError('HTTP recovery after mixed phase failed')
    result_rows = []
    for ip in targets:
        client = clients[ip]
        started = monotonic()
        deadline = started + RECOVERY_BUSY_TOTAL_TIMEOUT
        busy_attempts = []
        attempt_request_ids = []
        while True:
            remaining = deadline-monotonic()
            if remaining <= 0:
                runner.block(ip, 'MQTT recovery busy retry deadline expired')
                raise TimeoutError('MQTT recovery busy retry deadline expired')
            request_id = 'readonly-mixed-' + uuid.uuid4().hex
            attempt_request_ids.append(request_id)
            payload = make_status_payload(request_id)
            attempt_started = monotonic()
            try:
                info = client.client.publish(roots[ip]+'/command', json.dumps(
                    payload, separators=(',', ':')).encode(), qos=1, retain=False)
            except Exception as exc:
                runner.block(ip, 'MQTT recovery publish failed: '+type(exc).__name__)
                raise RuntimeError('MQTT recovery publish failed') from exc
            if info.rc != 0:
                runner.block(ip, 'MQTT recovery QoS1 publish failed')
                raise RuntimeError('MQTT recovery publish failed')
            remaining = deadline-monotonic()
            if remaining <= 0:
                runner.block(ip, 'MQTT recovery publish exceeded busy retry deadline')
                raise TimeoutError('MQTT recovery publish exceeded busy retry deadline')
            try:
                info.wait_for_publish(timeout=min(MQTT_PUBACK_TIMEOUT, remaining))
            except Exception as exc:
                runner.block(ip, 'MQTT recovery PUBACK wait failed: '+type(exc).__name__)
                raise RuntimeError('MQTT recovery PUBACK wait failed') from exc
            if not info.is_published():
                runner.block(ip, 'MQTT recovery PUBACK timed out')
                raise TimeoutError('MQTT recovery PUBACK timed out')
            remaining = deadline-monotonic()
            if remaining <= 0:
                runner.block(ip, 'MQTT recovery PUBACK exceeded busy retry deadline')
                raise TimeoutError('MQTT recovery PUBACK exceeded busy retry deadline')
            try:
                topic, body, retained, raw = receive_correlated(
                    client, request_id, roots[ip]+'/result', roots[ip]+'/state',
                    min(MQTT_RESULT_TIMEOUT, remaining), evidence.data['telemetry'],
                    ip, name, runner, collection_stage='recovery',
                    window_started=monotonic(),
                    absolute_deadline=min(deadline, monotonic()+MQTT_RESULT_TIMEOUT),
                    include_raw=True, monotonic=monotonic)
            except MQTTProtocolError as exc:
                evidence.append('protocol_errors', {
                    'ip': ip, 'phase': name, 'topic_kind': 'recovery',
                    'error': str(exc),
                })
                runner.block(ip, 'MQTT recovery protocol error')
                raise
            except (queue.Empty, TimeoutError, ConnectionError) as exc:
                runner.block(ip, 'MQTT recovery response timed out')
                raise TimeoutError('MQTT recovery response timed out') from exc
            now = monotonic()
            if now >= deadline:
                runner.block(ip, 'MQTT recovery response exceeded busy retry deadline')
                raise TimeoutError('MQTT recovery response exceeded busy retry deadline')
            category = classify_mqtt_message(topic, body, retained, request_id,
                                             roots[ip]+'/result', roots[ip]+'/state')
            if category == 'busy':
                try:
                    if not isinstance(raw, bytes):
                        raise ValueError('raw MQTT response was not bytes')
                    decoded = json.loads(raw.decode('utf-8'))
                    if decoded != body:
                        raise ValueError('raw MQTT response differed from parsed body')
                except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
                    evidence.append('protocol_errors', {
                        'ip': ip, 'phase': name, 'topic_kind': 'recovery',
                        'error': 'Invalid raw bytes for correlated busy response',
                    })
                    runner.block(ip, 'MQTT recovery busy response failed raw validation')
                    raise MQTTProtocolError('MQTT busy response raw validation failed') from exc
                attempt = {
                    'ip': ip, 'phase': name, 'request_id': request_id,
                    'attempt': len(attempt_request_ids),
                    'retry_limit': RECOVERY_BUSY_MAX_RETRIES,
                    'attempt_limit': RECOVERY_BUSY_MAX_RETRIES+1,
                    'outcome': 'busy', 'topic': topic, 'retained': False,
                    'action': 'status.get', 'params': {},
                    'started_monotonic': attempt_started,
                    'ended_monotonic': now,
                    'elapsed_ms': (now-attempt_started)*1000,
                    'total_deadline_seconds': RECOVERY_BUSY_TOTAL_TIMEOUT,
                    'logical_deadline_monotonic': deadline,
                    'body_bytes': len(raw),
                    'raw_json_base64': base64.b64encode(raw).decode('ascii'),
                    'retry_exhausted': len(busy_attempts) >= RECOVERY_BUSY_MAX_RETRIES,
                    'deadline_exhausted': False,
                }
                busy_attempts.append(attempt)
                evidence.data['recovery_attempts'].append(attempt)
                evidence.save()
                if attempt['retry_exhausted']:
                    runner.block(ip, 'MQTT recovery exhausted matching busy retries')
                    raise RuntimeError('MQTT recovery exhausted matching busy retries')
                remaining = deadline-monotonic()
                if remaining <= RECOVERY_BUSY_RETRY_DELAY:
                    attempt['deadline_exhausted'] = True
                    evidence.save()
                    runner.block(ip, 'MQTT recovery busy retry deadline expired')
                    raise TimeoutError('MQTT recovery busy retry deadline expired')
                sleep(min(RECOVERY_BUSY_RETRY_DELAY, remaining))
                if monotonic() >= deadline:
                    attempt['deadline_exhausted'] = True
                    evidence.save()
                    runner.block(ip, 'MQTT recovery busy retry deadline expired')
                    raise TimeoutError('MQTT recovery busy retry deadline expired')
                continue
            if category != 'success':
                runner.block(ip, 'MQTT recovery result was not fresh correlated success')
                raise RuntimeError('MQTT recovery result failed correlation')
            try:
                summary = _validate_device_data(
                    body['data'], ip, runner.before[ip]['state'], runner.version)
            except (KeyError, ValueError, TypeError) as exc:
                runner.block(ip, 'MQTT recovery identity or state validation failed')
                raise RuntimeError('MQTT recovery result failed state validation') from exc
            break
        checked = dict(body['data'])
        checked.setdefault('device_id', summary['device_id'])
        runner.check(ip, checked, started, now)
        _observe_network(runner, checked, ip, name, 'mqtt-recovery',
                         attempt_started, can_restore=True)
        network_confirmation = None
        if checked['wifi_connected'] is not True or checked['mqtt_connected'] is not True:
            runner.block(ip, 'MQTT recovery status network is not ready')
        monitor = getattr(runner, 'mixed_network_readiness', None)
        if monitor is not None:
            network_state = monitor.state[ip]
            degraded_since = network_state['degraded_since_monotonic']
            not_ready_observed = network_state['last_not_ready_observed_monotonic']
            stale_correlated_status = (
                degraded_since is not None and
                not_ready_observed is not None and
                attempt_started < not_ready_observed and
                not network_state['expired'] and
                not runner.stop.is_set() and
                checked['wifi_connected'] is True and
                checked['mqtt_connected'] is True)
            if stale_correlated_status:
                confirmation_deadline = min(
                    deadline, degraded_since + NETWORK_NOT_READY_LIMIT)
                confirmation = {
                    'ip': ip, 'phase': name,
                    'reason': 'correlated-status-started-before-not-ready-observation',
                    'mqtt_request_started_monotonic': attempt_started,
                    'degraded_since_monotonic': degraded_since,
                    'not_ready_observed_monotonic': not_ready_observed,
                    'deadline_monotonic': confirmation_deadline,
                    'request_started_monotonic': None,
                    'request_ended_monotonic': None,
                    'outcome': 'pending',
                }
                evidence.append('network_confirmations', confirmation)
                evidence.save()
                if monotonic() >= confirmation_deadline:
                    confirmation.update(outcome='deadline_expired_before_request')
                    evidence.save()
                    runner.block(ip, 'Fresh HTTP network confirmation deadline expired')
                    raise TimeoutError('Fresh HTTP network confirmation deadline expired')
                try:
                    confirmation_row = runner.sample(
                        ip, name+'-http-network-confirmation', action='status.get',
                        params={'view': 'full'}, deadline=confirmation_deadline)
                except Exception as exc:
                    confirmation.update(outcome='request_error',
                                        error_type=type(exc).__name__)
                    evidence.save()
                    if not runner.stop.is_set():
                        runner.block(ip, 'Fresh HTTP network confirmation request failed: '+
                                     type(exc).__name__)
                    raise
                request_start = confirmation_row.get('request_started_monotonic')
                request_end = confirmation_row.get('request_ended_monotonic')
                confirmation.update(
                    request_started_monotonic=request_start,
                    request_ended_monotonic=request_end,
                    http=confirmation_row.get('http'),
                    code=confirmation_row.get('code'),
                    request_ok=confirmation_row.get('ok') is True,
                    wifi_connected=confirmation_row.get('wifi_connected'),
                    mqtt_connected=confirmation_row.get('mqtt_connected'),
                    uptime_seconds=confirmation_row.get('uptime'))
                latest_not_ready_after_request = network_state[
                    'last_not_ready_observed_monotonic']
                confirmation['latest_not_ready_observed_monotonic_after_request'] = \
                    latest_not_ready_after_request
                evidence.save()
                valid_times = (
                    type(request_start) in (int, float) and math.isfinite(request_start) and
                    type(request_end) in (int, float) and math.isfinite(request_end) and
                    request_end >= request_start and
                    request_end <= confirmation_deadline)
                if not confirmation_row.get('ok') or not valid_times:
                    confirmation.update(outcome='failed_confirmation')
                    evidence.save()
                    if not runner.stop.is_set():
                        runner.block(ip, 'Fresh HTTP network confirmation failed')
                    raise RuntimeError('Fresh HTTP network confirmation failed')
                try:
                    confirmed_network = _network_fields(confirmation_row)
                except ValueError as exc:
                    confirmation.update(outcome='invalid_network_fields')
                    evidence.save()
                    runner.block(ip, 'Fresh HTTP network confirmation fields invalid')
                    raise RuntimeError('Fresh HTTP network confirmation failed') from exc
                if not all(confirmed_network.values()):
                    confirmation.update(outcome='not_ready')
                    evidence.save()
                    runner.block(ip, 'Fresh HTTP network confirmation is not ready')
                    raise RuntimeError('Fresh HTTP network confirmation is not ready')
                if request_start < latest_not_ready_after_request:
                    confirmation.update(outcome='failed_confirmation')
                    evidence.save()
                    runner.block(ip, 'Fresh HTTP network confirmation predates a new not-ready observation')
                    raise RuntimeError('Fresh HTTP network confirmation failed')
                confirmation.update(outcome='ready')
                evidence.save()
                network_confirmation = {
                    'reason': confirmation['reason'],
                    'degraded_since_monotonic': degraded_since,
                    'not_ready_observed_monotonic': not_ready_observed,
                    'latest_not_ready_observed_monotonic_after_request':
                        latest_not_ready_after_request,
                    'mqtt_request_started_monotonic': attempt_started,
                    'request_started_monotonic': request_start,
                    'request_ended_monotonic': request_end,
                    'deadline_monotonic': confirmation_deadline,
                    'outcome': confirmation['outcome'],
                }
            monitor.require_ready(name+'-mqtt-recovery', ip)
        if runner.stop.is_set():
            raise RuntimeError('MQTT recovery state gate failed')
        result_rows.append({'ip': ip, 'request_id': request_id, 'outcome': 'success',
                            'retained': False, 'elapsed_ms': (now-started)*1000,
                            'attempt_count': len(attempt_request_ids),
                            'busy_attempt_count': len(busy_attempts),
                            'attempt_request_ids': attempt_request_ids,
                            'network_confirmation': network_confirmation, **summary})
    evidence.data['recovery'].append({'phase': name, 'http_completed': True,
                                      'mqtt_results': result_rows,
                                      'telemetry_observation': {
                                          ip: _telemetry_observation(
                                              evidence.data['telemetry'], ip, name, 'recovery')
                                          for ip in targets
                                      },
                                      'completed': len(result_rows) == len(targets)})
    evidence.save()
    if runner.stop.is_set():
        raise RuntimeError('MQTT recovery state gate failed')


def execute(targets, version, label, profile, output, runtime_loader=load_runtime,
            plan_override=None, run_timeout=RUN_TIMEOUT):
    selected_targets = {ip: TARGETS[ip] for ip in targets}
    runner_targets = {ip: ident for ip, (ident, _model) in selected_targets.items()}
    if output.exists():
        raise FileExistsError('Refusing to overwrite mixed-run evidence')
    output.mkdir(mode=0o700, parents=True)
    plan = [dict(row) for row in (plan_override if plan_override is not None else phase_plan(profile))]
    evidence = Evidence(output/'mixed-evidence.json', label, version, profile, plan,
                        selected_targets)
    Runner, mqtt_helpers, MQTTClient, mqtt_root, mqtt_client_id = runtime_loader()
    runner = _network_observed_runner(Runner)(runner_targets, version, output/'http-runner', require_shared_core=True)
    stop_timer = threading.Timer(run_timeout, runner.stop.set)
    stop_timer.daemon = True
    stop_timer.start()
    clients = {}
    roots = {}
    initialized = False
    finish_called = False
    phase_failure = None
    network_stop = threading.Event()
    network_thread = None
    try:
        credentials = mqtt_helpers.load_credentials()
        runner.initialize()
        if runner.blockers or runner.stop.is_set():
            raise RuntimeError('HTTP baseline initialization was blocked')
        for ip, (ident, model) in selected_targets.items():
            baseline = runner.before[ip]
            if baseline['id'].lower() != ident.lower():
                raise ValueError('HTTP target identity mismatch')
            state, config = baseline['state'], baseline['config']
            if state.get('model') != model or state.get('version') != version:
                raise ValueError('HTTP target model/version mismatch')
            if not mqtt_helpers.validate_broker_config(config, credentials):
                raise ValueError('HTTP broker config does not match private credentials')
            if state.get('wifi_connected') is not True or state.get('mqtt_connected') is not True:
                raise ValueError('HTTP baseline connectivity is not healthy')
        evidence.data['http_prechecks'] = 'passed'
        initialized = True
        runner.mixed_network_readiness = NetworkReadiness(
            runner, evidence, targets=selected_targets)
        def monitor_network():
            try:
                while not network_stop.wait(.05):
                    _check_network(runner, 'network-monitor')
            except BaseException as exc:
                runner.block('', 'Network observation monitor failed: '+type(exc).__name__)
        thread = threading.Thread(target=monitor_network, name='mixed-network-monitor', daemon=True)
        thread.start()
        network_thread = thread
        evidence.save()
        for ip, (ident, model) in selected_targets.items():
            root = mqtt_root(credentials['username'], model, ident)
            client_id = mqtt_client_id(credentials['username'], model, ident) + '_mix_' + uuid.uuid4().hex[:8]
            client = MQTTClient(credentials['host'], credentials['port'], client_id,
                                username=credentials['username'], password=credentials['password'],
                                timeout=5, evidence_dir=output)
            client.messages = BoundedMessageQueue(maximum=MESSAGE_QUEUE_LIMIT)
            clients[ip], roots[ip] = client, root
            client.subscribe(root+'/result')
            client.subscribe(root+'/state')
        evidence.data['mqtt_connected'] = True
        evidence.save()
        runner.memory_window('baseline')
        for item in plan:
            phase_abort = run_phase(runner, clients, roots, evidence, item, profile,
                                    targets=selected_targets)
            try:
                for ip in selected_targets:
                    runner.memory_sample(ip, item['name']+'-post')
                    sample = next((row for row in reversed(runner.memory)
                                   if row.get('ip') == ip and row.get('phase') == item['name']+'-post'), {})
                    evidence.data.setdefault('diagnostic_samples', []).append({
                        'ip': ip, 'phase': item['name'],
                        'values': {key: sample.get('data', {}).get(key) for key in
                                   QUEUE_FIELDS+STACK_FIELDS+('json_live_blocks', 'memory_active',
                                       'memory_reserved', 'memory_waiting', 'free_memory',
                                       'http_core_owner_state', 'http_core_restarts',
                                       'http_core_close_failures') if key in sample.get('data', {})},
                        'availability': optional_diagnostics([sample]),
                    })
                recover(runner, clients, roots, evidence, item['name'],
                        targets=selected_targets)
            except BaseException as exc:
                phase_failure = 'Read-only phase recovery failed: '+type(exc).__name__
            if phase_abort and phase_failure is None:
                phase_failure = 'Mixed phase incomplete; read-only recovery completed, retest required: '+phase_abort
            if runner.stop.is_set() and phase_failure is None:
                phase_failure = 'Runner gate stopped after recovery'
            if phase_failure:
                break
        drop_totals = {ip: getattr(clients[ip].messages, 'dropped', 0)
                       for ip in selected_targets}
        evidence.data['host_queue_drop_totals'] = drop_totals
        if any(drop_totals.values()) and phase_failure is None:
            runner.block('', 'Bounded host MQTT message queue dropped messages')
            phase_failure = 'Bounded host MQTT message queue overflowed'
        for client in clients.values():
            client.disconnect()
        clients.clear()
        finish_failure = None
        if initialized:
            finish_called = True
            try:
                runner.finish()
            except BaseException as exc:
                finish_failure = 'HTTP final restore/idle/config gates failed: '+type(exc).__name__
        if phase_failure:
            runner.block('', phase_failure)
        if finish_failure:
            runner.block('', finish_failure)
        runner.mixed_network_readiness.require_ready('final-network-gate')
        if (not initialized or runner.run.get('completed') is not True or runner.blockers or
                any(value is not True for value in runner.completion_checks.values())):
            reason = phase_failure or finish_failure or 'HTTP final restore/idle/config gates failed'
            runner.save_interrupted(RuntimeError(reason))
            raise RuntimeError(reason)
        evidence.data['http_postchecks'] = 'passed'
        evidence.data['diagnostic_availability'] = optional_diagnostics(runner.memory)
        evidence.data['status'] = 'complete'
        evidence.data['completed'] = True
        evidence.data['ended_at'] = time.time()
        evidence.save()
        return evidence.data
    except BaseException as exc:
        for client in clients.values():
            try:
                client.disconnect()
            except Exception:
                pass
        clients.clear()
        if initialized and not finish_called:
            finish_called = True
            try:
                runner.finish()
                evidence.data['exception_finalization'] = {
                    'attempted': True,
                    'run_status_after_finish': runner.run.get('status'),
                    'completed': runner.run.get('completed') is True,
                    'completion_checks': dict(runner.completion_checks),
                    'blocker_count': len(runner.blockers),
                }
            except BaseException as finish_exc:
                evidence.data['exception_finalization'] = {
                    'attempted': True,
                    'run_status_after_finish': runner.run.get('status'),
                    'completed': False,
                    'completion_checks': dict(runner.completion_checks),
                    'blocker_count': len(runner.blockers),
                    'failure_type': type(finish_exc).__name__,
                }
        evidence.data.update(status='failed', completed=False, ended_at=time.time(),
                             failure_type=type(exc).__name__,
                             blockers=[dict(row) for row in runner.blockers])
        runner.save_interrupted(exc)
        evidence.save()
        raise
    finally:
        network_stop.set()
        if network_thread is not None:
            network_thread.join()
        stop_timer.cancel()
        for client in clients.values():
            try:
                client.disconnect()
            except Exception:
                pass


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', action='append', required=True, metavar='IP=ID')
    parser.add_argument('--version', required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--profile', choices=('short', 'soak'), default='short')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--execute', action='store_true')
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        targets = validate_targets(args.target)
        version = validate_version(args.version)
        if not args.label or len(args.label) > 48 or not args.label.replace('-', '').replace('_', '').isalnum():
            raise ValueError('Invalid label')
        output = (args.output or PRIVATE/(args.label+'-mqtt-http-mixed')).resolve()
        if PRIVATE.resolve() not in output.parents:
            raise ValueError('Evidence must stay under build/status101/private')
        if not args.execute:
            print(json.dumps({
                'mode': 'dry-run', 'targets': targets, 'version': version,
                'label': args.label, 'profile': args.profile, 'plan': phase_plan(args.profile),
                'http_period_seconds': HTTP_PERIOD, 'mqtt_max_outstanding_per_device': 4,
                'mqtt_qos': 1, 'mqtt_retain': False, 'action': 'status.get',
            'recovery': '3 consecutive HTTP status.get successes + 1 fresh correlated MQTT status.get per target',
            'mqtt_message_queue_limit': MESSAGE_QUEUE_LIMIT,
                'will_read_credentials': False, 'will_connect': False,
                'output': str(output),
            }, sort_keys=True))
            return 0
        with run_lock():
            result = execute(targets, version, args.label, args.profile, output)
        print(json.dumps({'status': result['status'], 'completed': result['completed'],
                          'evidence': str(output/'mixed-evidence.json')}, sort_keys=True))
        return 0
    except BaseException as exc:
        print(json.dumps({'status': 'failed', 'error_type': type(exc).__name__}, sort_keys=True))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
