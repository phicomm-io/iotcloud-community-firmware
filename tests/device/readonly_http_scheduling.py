#!/usr/bin/env python3
"""Bounded read-only HTTP scheduling comparison for named devices.

The default mode prints an offline manifest. Device traffic requires --execute
and explicit IP=identity and firmware-version arguments.
"""
import argparse
import concurrent.futures
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import select
import socket
import sys
import threading
import time

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.device.readonly_input_edges import (  # noqa: E402
    AUTHORIZED_MODELS,
    AUTHORIZED_TARGETS,
)
from tests.device.readonly_stress import Runner  # noqa: E402


LOCK_PATH = '/tmp/phicomm-readonly-stress.lock'
CASES = ('baseline', 'incomplete-status-header')
LIVE_BODY = json.dumps(
    {'action': 'status.get', 'params': {'view': 'live'}},
    separators=(',', ':'),
).encode('ascii')
MAX_REQUEST_BYTES = 8192
MAX_RESPONSE_BYTES = 16384
PROBE_COUNT = 6
PROBE_CONCURRENCY = 2
PROBE_TIMEOUT = 5.0
CASE_DEADLINE = 18.0
SLOW_CONNECT_TIMEOUT = 2.0
SLOW_SETTLE_SECONDS = 0.2
SLOW_OBSERVE_SECONDS = 0.05
MAX_SLOW_PEEK_BYTES = 512
QUIET_GAP_SECONDS = 1.0
RECOVERY_TIMEOUT = 45.0


def validate_targets(target_args, version_args):
    targets = {}
    for value in target_args:
        if '=' not in value:
            raise ValueError('targets must use IP=expected_device_id')
        ip, ident = value.split('=', 1)
        try:
            ip = str(ipaddress.ip_address(ip))
        except ValueError as exc:
            raise ValueError('invalid target IP') from exc
        expected = AUTHORIZED_TARGETS.get(ip)
        if expected is None or ident.lower() != expected.lower():
            raise ValueError('target IP and device identity are not authorized')
        if ip in targets:
            raise ValueError('target IPs must be unique')
        targets[ip] = ident
    if not targets:
        raise ValueError('at least one explicit target is required')

    versions = {}
    for value in version_args:
        if '=' not in value:
            raise ValueError('target versions must use IP=version')
        ip, version = value.split('=', 1)
        if ip not in targets or ip in versions or not version:
            raise ValueError('version overrides must name unique explicit targets')
        versions[ip] = version
    return targets, versions


def build_probe_request(host):
    head = (
        f'POST /api/v1/command HTTP/1.1\r\n'
        f'Host: {host}\r\n'
        'Content-Type: application/json\r\n'
        f'Content-Length: {len(LIVE_BODY)}\r\n'
        'Connection: close\r\n\r\n'
    ).encode('ascii')
    wire = head + LIVE_BODY
    if len(wire) > MAX_REQUEST_BYTES:
        raise ValueError('probe request exceeds 8192-byte limit')
    return wire


def build_slow_header(host):
    wire = (
        f'GET /api/v1/status HTTP/1.1\r\n'
        f'Host: {host}\r\n'
        'X-Scheduling-Hold:'
    ).encode('ascii')
    if len(wire) > MAX_REQUEST_BYTES:
        raise ValueError('slow request exceeds 8192-byte limit')
    return wire


def parse_probe_response(raw, closed=False):
    """Parse bounded HTTP facts and honor Content-Length framing."""
    split = raw.find(b'\r\n\r\n')
    if split < 0:
        return {'status': None, 'body': b'', 'complete': False,
                'closed': closed, 'content_length': None}
    try:
        lines = raw[:split].split(b'\r\n')
        status_line = lines[0].decode('ascii').split()
        if len(status_line) < 2 or not status_line[0].startswith('HTTP/1.'):
            raise ValueError
        status = int(status_line[1])
        lengths = []
        for line in lines[1:]:
            key, sep, value = line.partition(b':')
            if not sep:
                raise ValueError
            if key.strip().lower() == b'content-length':
                lengths.append(value.strip())
        if len(lengths) > 1 or (lengths and not lengths[0].isdigit()):
            raise ValueError
    except (UnicodeDecodeError, ValueError):
        return {'status': None, 'body': b'', 'complete': False,
                'closed': closed, 'content_length': None}

    body_bytes = raw[split + 4:]
    content_length = int(lengths[0]) if lengths else None
    if content_length is None:
        body = body_bytes
        complete = closed
    else:
        body = body_bytes[:content_length]
        complete = len(body_bytes) >= content_length
    return {'status': status, 'body': body, 'complete': complete,
            'closed': closed, 'content_length': content_length}


def classify_response(status, body, complete):
    if status is None:
        return {'category': 'no_http', 'success': False}
    if status == 503:
        return {'category': 'http_503', 'success': False}
    if status != 200:
        return {'category': 'http_other', 'success': False}
    try:
        envelope = json.loads(body)
    except (TypeError, ValueError):
        envelope = None
    state = envelope.get('data') if isinstance(envelope, dict) else None
    valid = (complete and isinstance(envelope, dict) and
             envelope.get('code') == 'ok' and isinstance(state, dict))
    return {'category': 'success' if valid else 'illegal_200',
            'success': valid, 'state': state if valid else None}


def observe_slow_socket(sock, timeout=SLOW_OBSERVE_SECONDS):
    """Peek briefly for an early HTTP response or peer close without draining."""
    readable, _, _ = select.select([sock], [], [], max(0.0, timeout))
    if not readable:
        return {'state': 'pending_no_response', 'http_status': None,
                'response_bytes': 0}
    try:
        data = sock.recv(MAX_SLOW_PEEK_BYTES, socket.MSG_PEEK)
    except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
        return {'state': 'closed_reset', 'http_status': None,
                'response_bytes': 0}
    if not data:
        return {'state': 'closed_empty', 'http_status': None,
                'response_bytes': 0}
    status = None
    line = data.split(b'\r\n', 1)[0]
    try:
        parts = line.decode('ascii').split()
        if len(parts) >= 2 and parts[0].startswith('HTTP/1.'):
            status = int(parts[1])
    except (UnicodeDecodeError, ValueError):
        pass
    return {'state': 'early_response', 'http_status': status,
            'response_bytes': len(data)}


def aggregate_probes(rows):
    counts = {name: 0 for name in ('success', 'http_503', 'http_other',
                                    'no_http', 'illegal_200', 'stopped')}
    elapsed = []
    by_category = {name: [] for name in counts}
    for row in rows:
        category = row.get('category')
        if category not in counts:
            counts['no_http'] += 1
            category = 'no_http'
        else:
            counts[category] += 1
        if isinstance(row.get('elapsed_ms'), (int, float)):
            value = float(row['elapsed_ms'])
            elapsed.append(value)
            by_category[category].append(value)
    elapsed.sort()

    def percentile(values, p):
        if not values:
            return None
        position = (len(values) - 1) * p
        lower = int(position)
        upper = min(lower + 1, len(values) - 1)
        fraction = position - lower
        return values[lower] + (values[upper] - values[lower]) * fraction

    latency_by_category = {}
    for name, values in by_category.items():
        values.sort()
        latency_by_category[name] = {
            'count': len(values),
            'p50_ms': percentile(values, .50),
            'p95_ms': percentile(values, .95),
            'p99_ms': percentile(values, .99),
        }

    total = len(rows)
    return {
        'probe_count': total,
        'success_count': counts['success'],
        'success_rate': counts['success'] / total if total else 0.0,
        'http_503': counts['http_503'],
        'http_other': counts['http_other'],
        'no_http': counts['no_http'],
        'illegal_200': counts['illegal_200'],
        'stopped': counts['stopped'],
        'all_outcomes_p50_ms': percentile(elapsed, .50),
        'all_outcomes_p95_ms': percentile(elapsed, .95),
        'all_outcomes_p99_ms': percentile(elapsed, .99),
        'latency_by_category': latency_by_category,
    }


def _read_response(sock, deadline):
    raw = bytearray()
    closed = False
    while time.monotonic() < deadline and len(raw) <= MAX_RESPONSE_BYTES:
        remaining = deadline - time.monotonic()
        readable, _, _ = select.select([sock], [], [], max(0.0, remaining))
        if not readable:
            break
        try:
            chunk = sock.recv(min(4096, MAX_RESPONSE_BYTES + 1 - len(raw)))
        except socket.timeout:
            break
        if not chunk:
            closed = True
            break
        raw.extend(chunk)
        facts = parse_probe_response(bytes(raw), closed=False)
        if facts['complete']:
            return facts, len(raw)
    if len(raw) > MAX_RESPONSE_BYTES:
        raise ValueError('HTTP response exceeded bounded read limit')
    return parse_probe_response(bytes(raw), closed=closed), len(raw)


def probe_once(ip, runner, case_deadline, index):
    started_wall = time.time()
    started = time.monotonic()
    row = {'index': index, 'started_at': started_wall}
    if runner.stop.is_set():
        row.update(category='stopped', elapsed_ms=0.0,
                   ended_at=time.time(), http_status=None, response_bytes=0)
        return row
    deadline = min(case_deadline, started + PROBE_TIMEOUT)
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        row.update(category='no_http', error='deadline', http_status=None,
                   response_bytes=0)
    else:
        sock = None
        try:
            sock = socket.create_connection((ip, 80), timeout=min(remaining, 3.0))
            sock.settimeout(max(.01, deadline - time.monotonic()))
            sock.sendall(build_probe_request(ip))
            sock.shutdown(socket.SHUT_WR)
            facts, received = _read_response(sock, deadline)
            classified = classify_response(facts['status'], facts['body'], facts['complete'])
            row.update(category=classified['category'],
                       http_status=facts['status'], response_bytes=received)
            if classified['category'] == 'success':
                runner.check(ip, classified['state'], started)
            elif classified['category'] == 'illegal_200':
                runner.block(ip, 'HTTP 200 lacked a complete status.get ok envelope')
            elif classified['category'] == 'http_other':
                runner.block(ip, 'unexpected HTTP status: ' + str(facts['status']))
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError) as exc:
            row.update(category='no_http', error=type(exc).__name__,
                       http_status=None, response_bytes=0)
        except (OSError, ValueError) as exc:
            row.update(category='no_http', error=type(exc).__name__,
                       http_status=None, response_bytes=0)
        finally:
            if sock is not None:
                sock.close()
    row['elapsed_ms'] = round((time.monotonic() - started) * 1000, 3)
    row['ended_at'] = time.time()
    return row


def run_probe_batch(ip, runner, phase, case_deadline):
    jobs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=PROBE_CONCURRENCY) as pool:
        for index in range(PROBE_COUNT):
            jobs.append(pool.submit(probe_once, ip, runner, case_deadline, index))
        rows = [job.result() for job in jobs]
    for row in rows:
        row['phase'] = phase
    return rows


def run_case(ip, runner, phase, slow_connection=False, round_number=1):
    case_started_wall = time.time()
    case_started = time.monotonic()
    deadline = case_started + CASE_DEADLINE
    slow_sock = None
    slow_opened = None
    slow_settle = 0.0
    probes = []
    error = None
    slow_observations = []
    case_validity = 'BASELINE' if not slow_connection else 'PENDING_ADMISSION'
    slow_admitted = None
    try:
        if slow_connection:
            remaining = deadline - time.monotonic()
            slow_sock = socket.create_connection(
                (ip, 80), timeout=min(SLOW_CONNECT_TIMEOUT, max(.01, remaining)))
            slow_sock.settimeout(min(SLOW_CONNECT_TIMEOUT, max(.01, deadline-time.monotonic())))
            slow_sock.sendall(build_slow_header(ip))
            slow_opened = time.monotonic()
            settle_for = min(SLOW_SETTLE_SECONDS, max(0.0, deadline-time.monotonic()))
            settle_started = time.monotonic()
            if settle_for:
                time.sleep(settle_for)
            slow_settle = time.monotonic() - settle_started
            observe_timeout = min(SLOW_OBSERVE_SECONDS,
                                  max(0.0, deadline-time.monotonic()))
            initial_observation = observe_slow_socket(slow_sock, observe_timeout)
            slow_observations.append({'stage': 'after_settle', **initial_observation})
            if initial_observation['state'] != 'pending_no_response':
                slow_admitted = False
                case_validity = 'NOT_EXERCISED'
            else:
                probes = run_probe_batch(ip, runner, phase, deadline)
                observe_timeout = min(SLOW_OBSERVE_SECONDS,
                                      max(0.0, deadline-time.monotonic()))
                final_observation = observe_slow_socket(slow_sock, observe_timeout)
                slow_observations.append({'stage': 'after_probes', **final_observation})
                case_validity = 'PROBES_RUN_ADMISSION_UNCONFIRMED'
        else:
            probes = run_probe_batch(ip, runner, phase, deadline)
    except (OSError, ValueError) as exc:
        error = type(exc).__name__
        runner.block(ip, 'could not complete bounded scheduling case: ' + error)
    finally:
        slow_hold = None
        if slow_sock is not None:
            if slow_opened is not None:
                slow_hold = time.monotonic() - slow_opened
            slow_sock.close()
    case_ended = time.monotonic()
    result = {
        'ip': ip,
        'round': round_number,
        'phase': phase,
        'started_at': case_started_wall,
        'ended_at': time.time(),
        'wall_seconds': round(case_ended - case_started, 3),
        'slow_connection': slow_connection,
        'slow_admitted': slow_admitted,
        'case_validity': case_validity,
        'slow_socket_observations': slow_observations,
        'slow_settle_seconds': round(slow_settle, 3),
        'slow_hold_seconds': round(slow_hold, 3) if slow_hold is not None else 0.0,
        'probes': probes,
        'summary': aggregate_probes(probes),
    }
    if error:
        result['error'] = error
    if case_ended - case_started > CASE_DEADLINE:
        runner.block(ip, 'scheduling case exceeded its 18-second deadline')
    if any(row.get('category') == 'illegal_200' for row in probes):
        runner.block(ip, 'illegal HTTP 200 status response')
    if any(row.get('category') == 'http_other' for row in probes):
        runner.block(ip, 'unexpected HTTP status in scheduling probe')
    runner.scheduling_cases.append(result)
    return result


def recover_three_status_reads(ip, runner, label):
    deadline = time.monotonic() + RECOVERY_TIMEOUT
    good = 0
    reads = 0
    while good < 3 and time.monotonic() < deadline:
        row = runner.sample(ip, label)
        reads += 1
        good = good + 1 if row.get('ok') else 0
        if good < 3:
            time.sleep(min(.25, max(0.0, deadline-time.monotonic())))
    if good < 3:
        runner.block(ip, 'three complete status.get recovery reads did not succeed')
    return {'reads': reads, 'consecutive_successes': good,
            'recovered': good >= 3, 'ended_at': time.time()}


class SchedulingRunner(Runner):
    def __init__(self, targets, version, output, versions=None, rounds=1):
        super().__init__(targets, version, output, versions)
        self.rounds = rounds
        self.scheduling_cases = []
        self.scheduling_recovery = []

    def check(self, ip, state, start):
        super().check(ip, state, start)
        model = self.before[ip]['state'].get('model')
        if model is not None and state.get('model') is not None and state.get('model') != model:
            self.block(ip, 'device model changed')

    def save_scheduling(self):
        data = {
            'version': self.version,
            'target_versions': self.versions,
            'targets': list(self.targets.items()),
            'rounds': self.rounds,
            'cases': self.scheduling_cases,
            'recovery': self.scheduling_recovery,
            'blockers': self.blockers[:],
        }
        path = self.output / 'http-scheduling.json'
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
        fd = os.open(path, flags, 0o600)
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, 'w', encoding='utf-8') as stream:
                json.dump(data, stream, indent=2)
                stream.write('\n')
        except Exception:
            try:
                os.close(fd)
            except OSError:
                pass
            raise

    def run_comparison(self):
        for ip in self.targets:
            if self.stop.is_set():
                break
            if self.before[ip]['state'].get('model', '').lower() != AUTHORIZED_MODELS[ip]:
                self.block(ip, 'baseline model does not match authorized target')
                break
            for round_number in range(1, self.rounds + 1):
                if self.stop.is_set():
                    break
                run_case(ip, self, 'baseline', round_number=round_number)
                if self.stop.is_set():
                    break
                time.sleep(QUIET_GAP_SECONDS)
                run_case(ip, self, 'incomplete-status-header',
                         slow_connection=True, round_number=round_number)
                recovery = recover_three_status_reads(
                    ip, self, f'round-{round_number}-incomplete-status-header-recovery')
                self.scheduling_recovery.append(
                    {'ip': ip, 'round': round_number, **recovery})
                if not recovery['recovered'] or self.stop.is_set():
                    break
                time.sleep(QUIET_GAP_SECONDS)
                self.save()
                self.save_scheduling()
            if self.stop.is_set():
                break
        self.save_scheduling()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', action='append', required=True,
                        help='Explicit authorized IP=expected_device_id')
    parser.add_argument('--version', required=True, help='Expected firmware version')
    parser.add_argument('--target-version', action='append', default=[],
                        help='Optional IP=version override for an explicit target')
    parser.add_argument('--output', type=Path, default=Path('readonly-http-scheduling-evidence'))
    parser.add_argument('--rounds', type=int, choices=(1, 2, 3), default=1,
                        help='Complete baseline/competition/recovery rounds (default: 1)')
    parser.add_argument('--execute', action='store_true',
                        help='Run bounded read-only status probes; default is dry-run')
    args = parser.parse_args(argv)
    if not args.version:
        parser.error('--version must not be empty')
    try:
        targets, versions = validate_targets(args.target, args.target_version)
    except ValueError as exc:
        parser.error(str(exc))

    manifest = {
        'mode': 'execute' if args.execute else 'dry-run',
        'rounds': args.rounds,
        'quiet_gap_seconds': QUIET_GAP_SECONDS,
        'targets': [{'ip': ip, 'device_id': ident,
                     'model': AUTHORIZED_MODELS[ip],
                     'cases': list(CASES), 'probes_per_case': PROBE_COUNT,
                     'max_probe_concurrency': PROBE_CONCURRENCY,
                     'probe_timeout_seconds': PROBE_TIMEOUT,
                     'case_deadline_seconds': CASE_DEADLINE,
                     'slow_settle_seconds': SLOW_SETTLE_SECONDS,
                     'max_request_bytes': MAX_REQUEST_BYTES}
                    for ip, ident in targets.items()],
    }
    if not args.execute:
        print(json.dumps(manifest, indent=2))
        return 0

    evidence_path = args.output / 'http-scheduling.json'
    runner_evidence = args.output / 'evidence.json'
    if args.output.exists() or evidence_path.exists() or runner_evidence.exists():
        parser.error('output path already exists; refusing to overwrite evidence')

    with open(LOCK_PATH, 'a') as guard:
        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        runner = SchedulingRunner(targets, args.version, args.output, versions,
                                  rounds=args.rounds)
        runner.initialize()
        runner.run_comparison()
        runner.finish()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
