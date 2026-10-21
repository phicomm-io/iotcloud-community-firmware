#!/usr/bin/env python3
"""Bounded HTTP/MQTT read-only mixed-load runner for the three shared candidates.

All broker publications are QoS 1, non-retained status.get requests. Dry-run is
the default. Device failure injection and broker disconnects belong in host tests.
"""
import argparse
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
RUN_TIMEOUT = 15000.0
OUTCOME_NAMES = ('success', 'busy', 'timeout', 'transport', 'other', 'cancelled')
QUEUE_FIELDS = (
    'mqtt_pending', 'mqtt_peak', 'mqtt_overflow', 'mqtt_max_delay_ms',
    'mqtt_rx_queued', 'mqtt_tx_queued', 'mqtt_inflight', 'mqtt_queue_bytes',
    'mqtt_peak_bytes', 'mqtt_credit_bytes', 'mqtt_rejected', 'mqtt_owner_faults',
    'mqtt_generation', 'mqtt_rx_buffer_bytes', 'mqtt_tx_buffer_bytes',
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
    if parsed != expected:
        raise ValueError('Explicit targets must exactly match the authorized three devices')
    return parsed


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
    def __init__(self, path, label, version, profile, plan):
        self.path = path
        self.data = {
            'schema': 1, 'label': label, 'version': version, 'profile': profile,
            'status': 'running', 'completed': False, 'started_at': time.time(),
            'targets': {ip: {'device_id': ident, 'model': model}
                        for ip, (ident, model) in TARGETS.items()},
            'plan': plan, 'phases': [], 'http': [], 'mqtt': [], 'telemetry': [],
            'recovery': [], 'protocol_errors': [], 'unmatched_mqtt': [],
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
    for key in controls + ('wifi_connected', 'mqtt_connected'):
        if key not in initial or key not in data or data[key] != initial[key]:
            raise ValueError('MQTT status control/connectivity mismatch: '+key)
    uptime, base_uptime = data.get('uptime_seconds'), initial.get('uptime_seconds')
    if (type(uptime) not in (int, float) or not math.isfinite(uptime) or
            type(base_uptime) not in (int, float) or uptime < base_uptime):
        raise ValueError('MQTT status uptime missing or below baseline')
    if data.get('watchdog_state') not in (None, 'healthy', 'starting'):
        raise ValueError('MQTT status watchdog is unhealthy')
    return {'device_id': reported_id, 'model': model, 'version': version,
            'uptime_seconds': uptime}


class DeviceMQTTWorker:
    def __init__(self, ip, client, root, runner, evidence, stop):
        self.ip, self.client, self.root = ip, client, root
        self.runner, self.evidence, self.stop = runner, evidence, stop
        self.failure = None
        self.phase_abort_reason = None
        self.phase_started = time.monotonic()
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
        next_send = time.monotonic()
        sending_done = False
        while (not sending_done or pending.pending) and not self.stop.is_set() and not self.runner.stop.is_set():
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
                            'fresh': False, 'reason': 'initial retained snapshot excluded',
                        })
                    elif isinstance(body, dict):
                        try:
                            data = body.get('data', body)
                            identity = _validate_device_data(
                                data, self.ip, self.runner.before[self.ip]['state'], self.runner.version)
                            self.evidence.append('telemetry', {
                                'ip': self.ip, 'phase': phase, 'retained': False,
                                'fresh': True, 'validated': True,
                                'received_monotonic': now,
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
                            'fresh': False, 'reason': 'protocol error: non-object state payload',
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


def run_phase(runner, clients, roots, evidence, phase_plan, profile):
    plan = phase_plan
    phase = plan['name']
    started = time.monotonic()
    deadline, hard_deadline = phase_deadlines(started, plan['seconds'])
    http_before = len(runner.rows)
    dropped_before = {ip: getattr(clients[ip].messages, 'dropped', 0) for ip in TARGETS}
    phase_row = {**plan, 'started_at': time.time(), 'completed': False}
    phase_row['progress_checkpoints'] = []
    evidence.data['phases'].append(phase_row)
    mqtt_workers = []
    from concurrent.futures import ThreadPoolExecutor
    stop = threading.Event()
    pool = ThreadPoolExecutor(max_workers=plan['http_clients']*len(TARGETS))
    futures = []
    mqtt_threads = []
    hard_timed_out = False
    try:
        futures = [pool.submit(_http_worker, runner, ip, phase, deadline, n, stop, HTTP_PERIOD)
                   for ip in TARGETS for n in range(plan['http_clients'])]
        queue_overflow_lock = threading.Lock()
        queue_overflow_seen = set()
        for ip in TARGETS:
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
                for ip in TARGETS:
                    runner.memory_sample(ip, phase)
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
    for worker in mqtt_workers:
        if worker.failure:
            runner.block('', worker.failure)
    dropped = {ip: getattr(clients[ip].messages, 'dropped', 0)-dropped_before[ip]
               for ip in TARGETS}
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
        phase_row['abort_reason'] = next(
            (worker.phase_abort_reason for worker in mqtt_workers if worker.phase_abort_reason),
            'Mixed phase ended before its requested duration')
    phase_http = [row for row in evidence.data['http'] if row.get('phase') == phase]
    phase_row['http_outcomes'] = summarize_latencies(phase_http)
    phase_row['mqtt_outcomes'] = summarize_latencies(
        [row for row in evidence.data['mqtt'] if row.get('phase') == phase])
    for ip in TARGETS:
        if not any(row.get('ip') == ip and row.get('outcome') == 'success' for row in phase_http):
            phase_row.setdefault('abort_reason', 'No successful HTTP status response for '+ip)
        if not any(row.get('ip') == ip and row.get('outcome') == 'success'
                   for row in evidence.data['mqtt'] if row.get('phase') == phase):
            if not any(row.get('ip') == ip and row.get('outcome') == 'busy'
                       for row in evidence.data['mqtt'] if row.get('phase') == phase):
                phase_row.setdefault('abort_reason', 'No correlated MQTT status response for '+ip)
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


def receive_correlated(client, request_id, result_topic, state_topic, timeout,
                       telemetry_events, ip, phase, runner):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        if not client.connected.is_set():
            raise ConnectionError('MQTT disconnected during phase recovery')
        try:
            topic, body, retained, _raw = client.messages.get(
                timeout=min(.1, max(.01, deadline-time.monotonic())))
        except queue.Empty:
            continue
        try:
            if topic == state_topic:
                if retained:
                    telemetry_events.append({
                        'ip': ip, 'phase': phase, 'retained': True, 'fresh': False,
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
                    telemetry_events.append({
                        'ip': ip, 'phase': phase, 'retained': False, 'fresh': True,
                        'validated': True, 'received_monotonic': time.monotonic(),
                        'uptime_source_seconds': identity['uptime_seconds'],
                        'uptime_reset_check': 'not_evaluated_without_source_timestamp',
                        **identity,
                    })
                continue
            if topic == result_topic:
                if not isinstance(body, dict):
                    raise MQTTProtocolError('Non-object MQTT result payload during recovery')
                if body.get('request_id') == request_id:
                    return topic, body, retained
        finally:
            client.messages.task_done()
    raise TimeoutError('Fresh correlated MQTT recovery result timed out')


def recover(runner, clients, roots, evidence, name):
    runner.recover(name+'-http', seconds=45)
    if runner.stop.is_set():
        raise RuntimeError('HTTP recovery after mixed phase failed')
    result_rows = []
    for ip in TARGETS:
        client = clients[ip]
        request_id = 'readonly-mixed-' + uuid.uuid4().hex
        payload = make_status_payload(request_id)
        started = time.monotonic()
        try:
            info = client.client.publish(roots[ip]+'/command', json.dumps(
                payload, separators=(',', ':')).encode(), qos=1, retain=False)
        except Exception as exc:
            runner.block(ip, 'MQTT recovery publish failed: '+type(exc).__name__)
            raise RuntimeError('MQTT recovery publish failed') from exc
        if info.rc != 0:
            runner.block(ip, 'MQTT recovery QoS1 publish failed')
            raise RuntimeError('MQTT recovery publish failed')
        try:
            info.wait_for_publish(timeout=MQTT_PUBACK_TIMEOUT)
        except Exception as exc:
            runner.block(ip, 'MQTT recovery PUBACK wait failed: '+type(exc).__name__)
            raise RuntimeError('MQTT recovery PUBACK wait failed') from exc
        if not info.is_published():
            runner.block(ip, 'MQTT recovery PUBACK timed out')
            raise TimeoutError('MQTT recovery PUBACK timed out')
        try:
            topic, body, retained = receive_correlated(
                client, request_id, roots[ip]+'/result', roots[ip]+'/state',
                MQTT_RESULT_TIMEOUT, evidence.data['telemetry'], ip, name, runner)
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
        now = time.monotonic()
        category = classify_mqtt_message(topic, body, retained, request_id,
                                         roots[ip]+'/result', roots[ip]+'/state')
        if category != 'success':
            runner.block(ip, 'MQTT recovery result was not fresh correlated success')
            raise RuntimeError('MQTT recovery result failed correlation')
        summary = _validate_device_data(body['data'], ip, runner.before[ip]['state'], runner.version)
        checked = dict(body['data'])
        checked.setdefault('device_id', summary['device_id'])
        runner.check(ip, checked, started, now)
        result_rows.append({'ip': ip, 'request_id': request_id, 'outcome': 'success',
                            'retained': False, 'elapsed_ms': (now-started)*1000, **summary})
    evidence.data['recovery'].append({'phase': name, 'http_completed': True,
                                      'mqtt_results': result_rows,
                                      'completed': len(result_rows) == len(TARGETS)})
    evidence.save()
    if runner.stop.is_set():
        raise RuntimeError('MQTT recovery state gate failed')


def execute(targets, version, label, profile, output, runtime_loader=load_runtime,
            plan_override=None, run_timeout=RUN_TIMEOUT):
    if output.exists():
        raise FileExistsError('Refusing to overwrite mixed-run evidence')
    output.mkdir(mode=0o700, parents=True)
    plan = [dict(row) for row in (plan_override if plan_override is not None else phase_plan(profile))]
    evidence = Evidence(output/'mixed-evidence.json', label, version, profile, plan)
    Runner, mqtt_helpers, MQTTClient, mqtt_root, mqtt_client_id = runtime_loader()
    runner = Runner(targets, version, output/'http-runner', require_shared_core=True)
    stop_timer = threading.Timer(run_timeout, runner.stop.set)
    stop_timer.daemon = True
    stop_timer.start()
    clients = {}
    roots = {}
    initialized = False
    phase_failure = None
    try:
        credentials = mqtt_helpers.load_credentials()
        runner.initialize()
        if runner.blockers or runner.stop.is_set():
            raise RuntimeError('HTTP baseline initialization was blocked')
        for ip, (ident, model) in TARGETS.items():
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
        evidence.save()
        for ip, (ident, model) in TARGETS.items():
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
            phase_abort = run_phase(runner, clients, roots, evidence, item, profile)
            try:
                for ip in TARGETS:
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
                recover(runner, clients, roots, evidence, item['name'])
            except BaseException as exc:
                phase_failure = 'Read-only phase recovery failed: '+type(exc).__name__
            if phase_abort and phase_failure is None:
                phase_failure = 'Mixed phase incomplete; read-only recovery completed, retest required: '+phase_abort
            if runner.stop.is_set() and phase_failure is None:
                phase_failure = 'Runner gate stopped after recovery'
            if phase_failure:
                break
        drop_totals = {ip: getattr(clients[ip].messages, 'dropped', 0) for ip in TARGETS}
        evidence.data['host_queue_drop_totals'] = drop_totals
        if any(drop_totals.values()) and phase_failure is None:
            runner.block('', 'Bounded host MQTT message queue dropped messages')
            phase_failure = 'Bounded host MQTT message queue overflowed'
        for client in clients.values():
            client.disconnect()
        clients.clear()
        finish_failure = None
        if initialized:
            try:
                runner.finish()
            except BaseException as exc:
                finish_failure = 'HTTP final restore/idle/config gates failed: '+type(exc).__name__
        if phase_failure:
            runner.block('', phase_failure)
        if finish_failure:
            runner.block('', finish_failure)
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
        evidence.data.update(status='failed', completed=False, ended_at=time.time(),
                             failure_type=type(exc).__name__,
                             blockers=[dict(row) for row in runner.blockers])
        runner.save_interrupted(exc)
        evidence.save()
        raise
    finally:
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
