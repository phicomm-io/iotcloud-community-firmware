#!/usr/bin/env python3
"""Bounded, read-only HTTP parser edge checks for explicitly named devices.

This script prints its case manifest without network access by default. Pass
--execute to run the cases. Every POST body is a status.get live query.
"""
import argparse
from dataclasses import dataclass
import fcntl
import http.client
import ipaddress
import json
import os
from pathlib import Path
import select
import socket
import sys
import time

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.device.readonly_stress import ProtocolError, Runner, request


AUTHORIZED_TARGETS = {
    '10.117.152.111': 'b0f893303134',
    '10.117.152.102': 'D0BAE463E12C',
    '10.117.152.110': 'b0f8932864b6',
}
AUTHORIZED_MODELS = {
    '10.117.152.111': 'a1',
    '10.117.152.102': 'tc1',
    '10.117.152.110': 'm1',
}
LOCK_PATH = '/tmp/phicomm-readonly-stress.lock'
MAX_REQUEST_BYTES = 8192
CONNECT_TIMEOUT = 3.0
IO_TIMEOUT = 2.0
CASE_DEADLINE = 12.0
DRIP_SECONDS = 10.0
DRIP_INTERVAL = 0.5
DRIP_BYTE_COUNT = int(DRIP_SECONDS / DRIP_INTERVAL) + 1
LIVE_BODY = json.dumps(
    {'action': 'status.get', 'params': {'view': 'live'}},
    separators=(',', ':'),
).encode('ascii')


@dataclass(frozen=True)
class Scenario:
    name: str
    wire: bytes
    drip_start: int = -1
    drip_end: int = -1


def _request(host, headers, *, path='/api/v1/command', version='HTTP/1.1',
             body=LIVE_BODY, length=None):
    if length is None:
        length = str(len(body))
    has_content_type = any(header.lower().startswith('content-type:')
                           for header in headers)
    lines = [f'POST {path} {version}', f'Host: {host}']
    if not has_content_type:
        lines.append('Content-Type: application/json')
    lines.extend([f'Content-Length: {length}',
             'Connection: close', *headers]
    )
    head = ('\r\n'.join(lines) + '\r\n\r\n').encode('latin-1')
    wire = head + body
    if len(wire) > MAX_REQUEST_BYTES:
        raise ValueError('scenario exceeds 8192-byte wire limit')
    return wire


def build_scenarios(host):
    """Build only the reviewed HTTP input cases, without opening a socket."""
    cases = [
        Scenario('user-agent-empty', _request(host, ['User-Agent:'])),
        Scenario('user-agent-compatible', _request(
            host, ['User-Agent: Mozilla/5.0 compatible'])),
        Scenario('user-agent-compatible-paren', _request(
            host, ['User-Agent: Mozilla/5.0 (compatible;)'])),
        Scenario('user-agent-compatible-msie', _request(
            host, ['User-Agent: Mozilla/5.0 (compatible; MSIE 8.0; Windows NT 6.0)'])),
        Scenario('user-agent-msie', _request(
            host, ['User-Agent: MSIE 8.0'])),
        Scenario('user-agent-invalid-octet', _request(
            host, ['User-Agent: Mozilla/5.0\x01compat'])),
        Scenario('content-type-long', _request(
            host, ['Content-Type: ' + ('a' * 5000)])),
        Scenario('content-length-negative', _request(
            host, [], length='-1')),
        Scenario('content-length-overflow', _request(
            host, [], length='18446744073709551615')),
        Scenario('request-line-long', _request(
            host, [], path='/api/v1/command' + ('x' * 4000))),
        Scenario('http-version-token-long', _request(
            host, [], version='HTTP/1.' + ('9' * 3000))),
        Scenario('headers-many', _request(
            host, [f'X-Edge-{i}: x' for i in range(96)])),
        Scenario('header-value-long', _request(
            host, ['X-Edge-Long: ' + ('x' * 4000)])),
    ]

    drip_head = (
        f'POST /api/v1/command HTTP/1.1\r\nHost: {host}\r\n'
        f'Content-Type: application/json\r\nContent-Length: {len(LIVE_BODY)}\r\n'
        'Connection: close\r\nX-Drip: '
    ).encode('ascii')
    drip_value = b'a' * DRIP_BYTE_COUNT
    drip_wire = drip_head + drip_value
    if len(drip_wire) > MAX_REQUEST_BYTES:
        raise ValueError('slow-header scenario exceeds 8192-byte wire limit')
    cases.append(Scenario('header-slow-drip', drip_wire,
                          len(drip_head), len(drip_head) + len(drip_value)))
    return tuple(cases)


def validate_targets(target_args, version_args):
    targets = {}
    for value in target_args:
        if '=' not in value:
            raise ValueError('targets must use IP=expected_device_id')
        ip, ident = value.split('=', 1)
        try:
            normalized_ip = str(ipaddress.ip_address(ip))
        except ValueError as exc:
            raise ValueError('invalid target IP') from exc
        authorized = AUTHORIZED_TARGETS.get(normalized_ip)
        if not authorized or ident.lower() != authorized.lower():
            raise ValueError('target IP and device identity are not authorized')
        if normalized_ip in targets:
            raise ValueError('target IPs must be unique')
        targets[normalized_ip] = ident

    if not targets:
        raise ValueError('at least one explicit target is required')

    versions = {}
    for value in version_args:
        if '=' not in value:
            raise ValueError('target versions must use IP=version')
        ip, target_version = value.split('=', 1)
        if ip not in targets or ip in versions or not target_version:
            raise ValueError('version overrides must name unique explicit targets')
        versions[ip] = target_version
    return targets, versions


def _observe_close(sock, deadline, initial=b''):
    received = len(initial)
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        readable, _, _ = select.select([sock], [], [], max(0.0, min(IO_TIMEOUT, remaining)))
        if not readable:
            continue
        try:
            data = sock.recv(1024)
        except (ConnectionResetError, BrokenPipeError):
            return True, received
        if not data:
            return True, received
        received += len(data)
    return False, received


def send_scenario(ip, scenario):
    """Send one bounded raw request and report transport facts only."""
    started = time.monotonic()
    deadline = started + CASE_DEADLINE
    result = {'name': scenario.name, 'wire_bytes': len(scenario.wire)}
    try:
        with socket.create_connection((ip, 80), timeout=CONNECT_TIMEOUT) as sock:
            sock.settimeout(IO_TIMEOUT)
            if scenario.drip_start < 0:
                sock.sendall(scenario.wire)
                sock.shutdown(socket.SHUT_WR)
                closed, received = _observe_close(sock, deadline)
                result['sent_seconds'] = round(time.monotonic() - started, 3)
            else:
                sock.sendall(scenario.wire[:scenario.drip_start])
                offset = scenario.drip_start
                while offset < scenario.drip_end and time.monotonic() < deadline:
                    wait_until = min(deadline, time.monotonic() + DRIP_INTERVAL)
                    while time.monotonic() < wait_until:
                        readable, _, _ = select.select(
                            [sock], [], [], max(0.0, min(.1, wait_until-time.monotonic())))
                        if readable:
                            data = sock.recv(1024)
                            if not data:
                                result['sent_seconds'] = round(time.monotonic()-started, 3)
                                result.setdefault('response_bytes', 0)
                                result['closed'] = True
                                result['close_seconds'] = round(time.monotonic()-started, 3)
                                return result
                            result['response_bytes'] = result.get('response_bytes', 0) + len(data)
                    sock.sendall(scenario.wire[offset:offset+1])
                    offset += 1
                result['sent_seconds'] = round(time.monotonic() - started, 3)
                if offset < scenario.drip_end:
                    result.update(closed=False, close_seconds=None)
                    return result
                # Leave the header line incomplete so only a server-initiated
                # timeout/close can satisfy this scenario.
                closed, received = _observe_close(sock, deadline)
            result['response_bytes'] = received
            result['closed'] = closed
            result['close_seconds'] = round(time.monotonic() - started, 3) if closed else None
    except socket.timeout as exc:
        result.update(error=type(exc).__name__, closed=False, close_seconds=None)
    except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError) as exc:
        result.update(error=type(exc).__name__, closed=True,
                      close_seconds=round(time.monotonic() - started, 3))
    except OSError as exc:
        result.update(error=type(exc).__name__, closed=False, close_seconds=None)
    return result


class InputRunner(Runner):
    """Reuse the stress runner's identity/state gates without config access."""

    def __init__(self, targets, version, output, versions=None):
        super().__init__(targets, version, output, versions)
        self.scenario_rows = []

    def initialize(self):
        for ip, ident in self.targets.items():
            status, reply, _, _, start = request(
                ip, 'status.get', timeout=IO_TIMEOUT)
            if status != 200 or reply.get('code') != 'ok':
                raise RuntimeError('baseline status.get failed')
            state = reply.get('data')
            if not isinstance(state, dict):
                raise RuntimeError('baseline status data missing')
            expected_version = self.versions.get(ip, self.version)
            if (state.get('device_id', '').lower() != ident.lower() or
                    state.get('version') != expected_version):
                raise RuntimeError('baseline identity or version mismatch')
            if state.get('model', '').lower() != AUTHORIZED_MODELS[ip]:
                raise RuntimeError('baseline model mismatch')
            if state.get('watchdog_state') not in (None, 'healthy'):
                raise RuntimeError('baseline watchdog is not healthy')
            uptime = state.get('uptime_seconds')
            if not isinstance(uptime, (int, float)):
                raise RuntimeError('baseline uptime missing')
            self.before[ip] = {
                'id': ident, 'state': state,
                'boot_origin': start - uptime,
            }
        self.save()

    def check(self, ip, state, start):
        super().check(ip, state, start)
        baseline = self.before[ip]['state']
        expected_model = baseline.get('model')
        if state.get('model') != expected_model:
            self.block(ip, 'device model changed')
        required = [key for key in ('wifi_connected', 'mqtt_connected')
                    if baseline.get(key) is True]
        for key in ('wifi_connected', 'mqtt_connected'):
            if type(state.get(key)) is not bool:
                self.block(ip, 'status connectivity field missing or invalid: ' + key)
        return all(state.get(key) is True for key in required)

    def sample(self, ip, phase, action='status.get', params=None, expected='ok'):
        if action != 'status.get' or params is not None:
            self.block(ip, 'non-full or non-status request rejected locally')
            return {'ok': False}
        row = {'ip': ip, 'phase': phase, 'action': 'status.get', 'at': time.time()}
        try:
            status, reply, size, elapsed, start = request(
                ip, 'status.get', timeout=IO_TIMEOUT)
            row.update(http=status, code=reply.get('code'), bytes=size, ms=elapsed,
                       ok=False)
            if status == 503:
                row['degraded'] = 'http_503'
            elif status != 200:
                self.block(ip, 'unexpected HTTP status: ' + str(status))
            elif reply.get('code') not in (expected, 'busy'):
                self.block(ip, 'unexpected command result: ' + str(reply.get('code')))
            elif reply.get('code') == 'busy':
                row['degraded'] = 'busy'
            else:
                state = reply.get('data')
                if not isinstance(state, dict):
                    self.block(ip, 'status.get data missing during recovery')
                else:
                    for key in ('uptime_seconds', 'model', 'wifi_connected',
                                'mqtt_connected', 'errors', 'last_error',
                                'watchdog_state'):
                        if key in state:
                            row[key] = state[key]
                    if 'uptime_seconds' in state:
                        row['uptime'] = state['uptime_seconds']
                    ready = self.check(ip, state, start)
                    row['network_ready'] = ready
                    row['ok'] = ready and not self.stop.is_set()
                    if not ready and not self.stop.is_set():
                        row['degraded'] = 'network_not_ready'
        except ProtocolError as exc:
            row.update(ok=False, http=exc.status, error=type(exc).__name__)
            self.block(ip, 'malformed successful HTTP response')
        except (OSError, ValueError, RuntimeError, http.client.HTTPException) as exc:
            row.update(ok=False, error=type(exc).__name__)
        with self.lock:
            self.rows.append(row)
        return row

    def save(self):
        data = {
            'version': self.version,
            'target_versions': self.versions,
            'targets': list(self.targets.items()),
            'blockers': self.blockers[:],
            'recoveries': self.recoveries[:],
            'scenarios': self.scenario_rows[:],
            'status_checks': self.rows[:],
            'baseline': {
                ip: {'device_id': before['id'],
                     'version': before['state'].get('version'),
                     'model': before['state'].get('model'),
                     'uptime_seconds': before['state'].get('uptime_seconds')}
                for ip, before in self.before.items()
            },
        }
        self.output.mkdir(parents=True, exist_ok=True)
        path = self.output / 'evidence.json'
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, 'w', encoding='utf-8') as stream:
                json.dump(data, stream, ensure_ascii=False, indent=2)
                stream.write('\n')
        except Exception:
            try:
                os.close(fd)
            except OSError:
                pass
            raise

    def finish(self):
        if not self.stop.is_set():
            self.restore_connectivity()
        if not self.stop.is_set():
            self.recover('final-recovery', seconds=20)
        self.save()
        if self.blockers:
            raise SystemExit(1)
        print('PASS all input cases closed within bounds and devices recovered')

    def run_scenarios(self):
        for ip in self.targets:
            for scenario in build_scenarios(ip):
                if self.stop.is_set():
                    break
                result = send_scenario(ip, scenario)
                row = {'ip': ip, **result}
                self.scenario_rows.append(row)
                if not result.get('closed'):
                    self.block(ip, 'input connection did not close within deadline')
                if (scenario.name == 'header-slow-drip' and result.get('closed') and
                        result.get('close_seconds', 0) > CASE_DEADLINE):
                    self.block(ip, 'slow-header close exceeded deadline')
                self.save()
                if self.stop.is_set():
                    break
                self.recover(scenario.name + '-recovery', seconds=30)
        self.finish()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', action='append', required=True,
                        help='Explicit authorized IP=expected_device_id')
    parser.add_argument('--version', required=True,
                        help='Expected firmware version')
    parser.add_argument('--target-version', action='append', default=[],
                        help='Optional IP=version override for an explicit target')
    parser.add_argument('--output', type=Path, default=Path('readonly-input-evidence'))
    parser.add_argument('--execute', action='store_true',
                        help='Send bounded read-only HTTP cases; default is no traffic')
    args = parser.parse_args(argv)
    if not args.version:
        parser.error('--version must not be empty')
    try:
        targets, versions = validate_targets(args.target, args.target_version)
    except ValueError as exc:
        parser.error(str(exc))

    scenarios = {ip: build_scenarios(ip) for ip in targets}
    manifest = [{'ip': ip, 'cases': [case.name for case in cases],
                 'max_request_bytes': max(len(case.wire) for case in cases)}
                for ip, cases in scenarios.items()]
    if not args.execute:
        print(json.dumps({'mode': 'dry-run', 'targets': manifest}, indent=2))
        return 0

    with open(LOCK_PATH, 'a') as guard:
        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        runner = InputRunner(targets, args.version, args.output, versions)
        runner.initialize()
        runner.run_scenarios()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
