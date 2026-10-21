#!/usr/bin/env python3
"""Read-only acceptance checks for the shared HTTP request parser.

Prints a bounded case manifest without network access by default. Device
traffic requires --execute and explicit authorized target identity/version.
"""
import argparse
import base64
from dataclasses import dataclass
import fcntl
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

from tests.device.readonly_input_edges import (  # noqa: E402
    AUTHORIZED_MODELS,
    AUTHORIZED_TARGETS,
    InputRunner,
    validate_targets,
)


LOCK_PATH = '/tmp/phicomm-readonly-stress.lock'
MAX_REQUEST_BYTES = 8192
MAX_RESPONSE_BYTES = 32768
CONNECT_TIMEOUT = 3.0
IO_TIMEOUT = 2.0
CASE_DEADLINE = 12.0
LIVE_BODY = json.dumps(
    {'action': 'status.get', 'params': {'view': 'live'}},
    separators=(',', ':'),
).encode('ascii')
CHUNKED_BODY = (format(len(LIVE_BODY), 'x').encode('ascii') + b'\r\n' +
                LIVE_BODY + b'\r\n0\r\n\r\n')


@dataclass(frozen=True)
class Scenario:
    name: str
    wire: bytes
    entity_body: bytes
    expect_success: bool


def _wire(host, headers, body=LIVE_BODY, *, request_line=b'POST /api/v1/command HTTP/1.1',
          request_ending=b'\r\n', header_separator=b'\r\n'):
    lines = [b'Host: ' + host.encode('ascii'),
             b'Content-Type: application/json', b'Connection: close', *headers]
    raw = request_line + request_ending + header_separator.join(lines) + b'\r\n\r\n' + body
    if len(raw) > MAX_REQUEST_BYTES:
        raise ValueError('scenario exceeds 8192-byte wire limit')
    return raw


def build_scenarios(host):
    """Construct the fixed read-only parser corpus without opening sockets."""
    cl = str(len(LIVE_BODY)).encode('ascii')
    cl_header = b'Content-Length: ' + cl
    cases = [
        Scenario('content-length-duplicate-equal', _wire(
            host, [cl_header, cl_header]), LIVE_BODY, False),
        Scenario('content-length-duplicate-different', _wire(
            host, [cl_header, b'Content-Length: ' + str(len(LIVE_BODY)+1).encode('ascii')]),
            LIVE_BODY, False),
        Scenario('transfer-encoding-chunked', _wire(
            host, [b'Transfer-Encoding: chunked'], CHUNKED_BODY), LIVE_BODY, False),
        Scenario('transfer-encoding-leading-ows', _wire(
            host, [b'Transfer-Encoding: \tchunked'], CHUNKED_BODY), LIVE_BODY, False),
        Scenario('transfer-encoding-trailing-ows', _wire(
            host, [b'Transfer-Encoding: chunked\t'], CHUNKED_BODY), LIVE_BODY, False),
        Scenario('content-length-plus-transfer-encoding', _wire(
            host, [cl_header, b'Transfer-Encoding: chunked'], CHUNKED_BODY),
            LIVE_BODY, False),
        Scenario('expect-100-continue', _wire(
            host, [cl_header, b'Expect: 100-continue']), LIVE_BODY, False),
        Scenario('request-line-bare-lf', _wire(
            host, [cl_header], request_ending=b'\n'), LIVE_BODY, False),
        Scenario('header-line-bare-lf', _wire(
            host, [b'X-Bare-LF: value\nContent-Length: ' + cl]), LIVE_BODY, False),
        Scenario('header-embedded-nul', _wire(
            host, [cl_header, b'X-Control: before\x00after']), LIVE_BODY, False),
        Scenario('header-embedded-control', _wire(
            host, [cl_header, b'X-Control: before\x01after']), LIVE_BODY, False),
        Scenario('header-cr-garbage', _wire(
            host, [cl_header, b'X-CR: value\rgarbage: x']), LIVE_BODY, False),
        Scenario('http-version-short', _wire(
            host, [cl_header], request_line=b'POST /api/v1/command HTTP/1'),
            LIVE_BODY, False),
        Scenario('legal-tab-header', _wire(
            host, [cl_header, b'X-Tab:\tvalue']), LIVE_BODY, True),
        Scenario('status-normal', _wire(host, [cl_header]), LIVE_BODY, True),
    ]
    return tuple(cases)


def validate_scenarios(cases):
    expected = {
        'content-length-duplicate-equal', 'content-length-duplicate-different',
        'transfer-encoding-chunked', 'transfer-encoding-leading-ows',
        'transfer-encoding-trailing-ows', 'content-length-plus-transfer-encoding',
        'expect-100-continue', 'request-line-bare-lf', 'header-line-bare-lf',
        'header-embedded-nul', 'header-embedded-control', 'header-cr-garbage',
        'http-version-short', 'legal-tab-header', 'status-normal',
    }
    if {case.name for case in cases} != expected:
        raise ValueError('scenario list does not match the reviewed allowlist')
    positive_names = {'legal-tab-header', 'status-normal'}
    for case in cases:
        if len(case.wire) > MAX_REQUEST_BYTES:
            raise ValueError('scenario exceeds 8192-byte wire limit')
        try:
            envelope = json.loads(case.entity_body)
        except (TypeError, ValueError) as exc:
            raise ValueError('scenario entity body is not allowlisted JSON') from exc
        if envelope != {'action': 'status.get', 'params': {'view': 'live'}}:
            raise ValueError('only status.get live request bodies are permitted')
        if case.expect_success != (case.name in positive_names):
            raise ValueError('scenario expectation does not match the reviewed allowlist')
    return cases


def parse_response_facts(raw, closed=False):
    """Parse a bounded HTTP response buffer and honor Content-Length framing."""
    offset = 0
    interim = []
    while True:
        end = raw.find(b'\r\n\r\n', offset)
        if end < 0:
            return {'status': None, 'interim_statuses': interim,
                    'complete': bool(closed), 'closed': closed,
                    'error': 'incomplete_headers' if raw else None,
                    'raw': raw[:MAX_RESPONSE_BYTES]}
        head_end = end + 4
        lines = raw[offset:end].split(b'\r\n')
        try:
            status_line = lines[0].decode('ascii')
            pieces = status_line.split(' ', 2)
            if len(pieces) < 2 or not pieces[0].startswith('HTTP/1.'):
                raise ValueError
            status = int(pieces[1])
            headers = {}
            for line in lines[1:]:
                if b':' not in line:
                    raise ValueError
                key, value = line.split(b':', 1)
                header_name = key.strip().decode('ascii').lower()
                header_value = value.strip().decode('latin-1')
                headers.setdefault(header_name, []).append(header_value)
        except (UnicodeDecodeError, ValueError):
            return {'status': None, 'interim_statuses': interim,
                    'complete': False, 'closed': closed,
                    'error': 'malformed_response_headers',
                    'raw': raw[:MAX_RESPONSE_BYTES]}
        body_start = head_end
        if 100 <= status < 200 and status != 101:
            interim.append(status)
            offset = body_start
            continue

        lengths = headers.get('content-length', [])
        content_length = None
        if lengths:
            if len(lengths) != 1 or not lengths[0].isdigit():
                return {'status': status, 'headers': headers,
                        'content_length': None, 'complete': False,
                        'closed': closed, 'error': 'invalid_content_length',
                        'raw': raw[:MAX_RESPONSE_BYTES]}
            content_length = int(lengths[0])
        body_buffer = raw[body_start:]
        if content_length is not None:
            body = body_buffer[:content_length]
            complete = len(body) == content_length
            raw_end = body_start + min(content_length, len(body_buffer))
        elif status in (204, 304):
            body, complete, raw_end = b'', True, body_start
        else:
            body, complete, raw_end = body_buffer, bool(closed), len(raw)
        return {
            'status': status,
            'interim_statuses': interim,
            'headers': headers,
            'content_length': content_length,
            'body': body,
            'complete': complete,
            'closed': closed,
            'error': None,
            'raw': raw[:min(raw_end, MAX_RESPONSE_BYTES)],
        }


def receive_response(sock, deadline):
    data = bytearray()
    closed = False
    timed_out = False
    too_large = False
    while time.monotonic() < deadline:
        # All requests ask for a short connection. A complete body alone does
        # not establish that the daemon released its accepted socket.
        remaining = deadline - time.monotonic()
        readable, _, _ = select.select([sock], [], [], max(0.0, min(IO_TIMEOUT, remaining)))
        if not readable:
            continue
        try:
            sock.settimeout(max(.001, min(IO_TIMEOUT, deadline-time.monotonic())))
            chunk = sock.recv(4096)
        except socket.timeout:
            timed_out = True
            break
        except ConnectionResetError:
            closed = True
            break
        if not chunk:
            closed = True
            break
        room = MAX_RESPONSE_BYTES - len(data)
        if len(chunk) > room:
            data.extend(chunk[:room])
            too_large = True
            break
        data.extend(chunk)
    else:
        timed_out = True
    facts = parse_response_facts(bytes(data), closed=closed)
    facts.update(timed_out=timed_out, too_large=too_large,
                 response_bytes=len(data))
    return facts


def send_scenario(ip, scenario, deadline=None):
    started = time.monotonic()
    deadline = min(started + CASE_DEADLINE, deadline) if deadline is not None else started + CASE_DEADLINE
    result = {'name': scenario.name, 'request_bytes': len(scenario.wire)}
    try:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise socket.timeout('case deadline')
        with socket.create_connection((ip, 80), timeout=min(CONNECT_TIMEOUT, remaining)) as sock:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout('case deadline')
            sock.settimeout(min(IO_TIMEOUT, remaining))
            sock.sendall(scenario.wire)
            if time.monotonic() >= deadline:
                raise socket.timeout('case deadline')
            sock.shutdown(socket.SHUT_WR)
            facts = receive_response(sock, deadline)
            raw = facts.pop('raw', b'')
            result.update(facts)
            result['raw_response_base64'] = base64.b64encode(raw).decode('ascii')
            result['elapsed_seconds'] = round(time.monotonic() - started, 3)
    except socket.timeout as exc:
        result.update(error=type(exc).__name__, timed_out=True, closed=False,
                      elapsed_seconds=round(time.monotonic() - started, 3))
    except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError) as exc:
        result.update(error=type(exc).__name__, timed_out=False, closed=True,
                      elapsed_seconds=round(time.monotonic() - started, 3))
    except OSError as exc:
        result.update(error=type(exc).__name__, timed_out=False, closed=False,
                      elapsed_seconds=round(time.monotonic() - started, 3))
    return result


def evaluate_scenario(scenario, result):
    """Classify the bounded response without touching device state."""
    status = result.get('status')
    closed = result.get('closed') is True
    if result.get('timed_out') or result.get('too_large') or not closed:
        return False, None
    if scenario.expect_success:
        try:
            envelope = json.loads(result.get('body', b''))
        except (TypeError, ValueError):
            return False, None
        state = envelope.get('data') if isinstance(envelope, dict) else None
        valid = (
            status == 200 and result.get('content_length') is not None and
            result.get('complete') is True and isinstance(envelope, dict) and
            envelope.get('code') == 'ok' and isinstance(state, dict)
        )
        return valid, state if valid else None
    rejected = (status is not None and 400 <= status <= 499 and
                (result.get('complete') is True or closed))
    transport_closed = status is None and closed and result.get('response_bytes', 0) == 0
    return status != 200 and (rejected or transport_closed), None


def retryable_legal_busy(scenario, result):
    """Only a complete, closed, strictly 503/busy legal response may retry."""
    if (not scenario.expect_success or result.get('status') != 503 or
            result.get('timed_out') or result.get('too_large') or
            result.get('closed') is not True or result.get('complete') is not True or
            result.get('content_length') is None):
        return False
    try:
        body = json.loads(result.get('body', b''))
    except (TypeError, ValueError):
        return False
    return isinstance(body, dict) and body.get('code') == 'busy'


class SharedHTTPRunner(InputRunner):
    def __init__(self, targets, version, output, versions=None):
        super().__init__(targets, version, output, versions)
        self.http_cases = []

    def save(self):
        data = {
            'version': self.version,
            'target_versions': self.versions,
            'targets': list(self.targets.items()),
            'blockers': self.blockers[:],
            'recoveries': self.recoveries[:],
            'http_cases': self.http_cases[:],
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
        path = self.output / 'shared-httpd-evidence.json'
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

    def run_scenarios(self):
        for ip in self.targets:
            for scenario in validate_scenarios(build_scenarios(ip)):
                if self.stop.is_set():
                    break
                deadline = time.monotonic()+15 if scenario.expect_success else None
                for attempt in range(1, 5):
                    started = time.monotonic()
                    result = (send_scenario(ip, scenario, deadline=deadline)
                              if deadline is not None else send_scenario(ip, scenario))
                    busy = retryable_legal_busy(scenario, result)
                    response_body = result.pop('body', b'')
                    row = {'ip': ip, **result, 'attempt': attempt,
                           'busy_rejection': busy}
                    self.http_cases.append(row)
                    accepted, state = evaluate_scenario(
                        scenario, {**result, 'body': response_body})
                    if deadline is not None and time.monotonic() >= deadline:
                        accepted, state = False, None
                        row['required_deadline_exceeded'] = True
                    self.save()
                    if (not busy or attempt == 4 or self.stop.is_set() or
                            deadline is None or time.monotonic()+1 >= deadline):
                        break
                    if self.stop.wait(1):
                        break
                failure_reason = None
                if not accepted:
                    failure_reason = (
                        'legal status.get request lacked valid HTTP 200 envelope'
                        if scenario.expect_success else
                        'malformed input was not rejected with 4xx or transport close')
                elif state is not None:
                    self.check(ip, state, started)
                if result.get('timed_out') or result.get('too_large'):
                    failure_reason = 'bounded response read timed out or exceeded 32768 bytes'
                if self.stop.is_set():
                    self.save()
                    break
                self.recover(scenario.name + '-recovery', seconds=30)
                if failure_reason and not self.stop.is_set():
                    self.block(ip, failure_reason)
                self.save()
                if self.stop.is_set():
                    break
        self.finish()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', action='append', required=True,
                        help='Explicit authorized IP=expected_device_id')
    parser.add_argument('--version', required=True, help='Expected firmware version')
    parser.add_argument('--target-version', action='append', default=[],
                        help='Optional IP=version override for an explicit target')
    parser.add_argument('--output', type=Path, default=Path('shared-httpd-evidence'))
    parser.add_argument('--execute', action='store_true',
                        help='Run the read-only raw HTTP acceptance cases')
    args = parser.parse_args(argv)
    if not args.version:
        parser.error('--version must not be empty')
    try:
        targets, versions = validate_targets(args.target, args.target_version)
    except ValueError as exc:
        parser.error(str(exc))

    manifests = []
    for ip in targets:
        scenarios = validate_scenarios(build_scenarios(ip))
        manifests.append({'ip': ip, 'model': AUTHORIZED_MODELS[ip],
                          'cases': [{'name': case.name,
                                     'expect': 'status.get ok' if case.expect_success else '4xx or close',
                                     'request_bytes': len(case.wire)}
                                    for case in scenarios]})
    if not args.execute:
        print(json.dumps({'mode': 'dry-run', 'targets': manifests}, indent=2))
        return 0

    with open(LOCK_PATH, 'a') as guard:
        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        runner = SharedHTTPRunner(targets, args.version, args.output, versions)
        runner.initialize()
        runner.run_scenarios()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
