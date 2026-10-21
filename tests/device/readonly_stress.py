#!/usr/bin/env python3
"""Bounded HTTP read-only stress of explicitly identified devices.

Overload rejection and transport errors are counted, not mistaken for resets.
A reset, control/config change, unhealthy watchdog, or failed recovery stops the
run. No write commands, MQTT configuration changes, or reboot requests exist.
"""
import argparse
import base64
import concurrent.futures
import fcntl
import http.client
import json
import math
from pathlib import Path
import signal
import socket
import statistics
import struct
import uuid
import threading
import time

ACTIONS = ('status.get', 'capabilities.get', 'logs.get', 'config.get',
           'power.get', 'binding.status.get')
CONTROL_KEYS_BY_MODEL = {
    'a1': ('requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled'),
    'tc1': ('socket_states', 'child_lock', 'led_enabled'),
    'm1': ('brightness_requested',),
}


class ProtocolError(ValueError):
    def __init__(self, status, raw):
        super().__init__("Malformed successful HTTP command response")
        self.status, self.raw = status, raw


class InitializationError(RuntimeError):
    def __init__(self, ip, action, reason):
        super().__init__('Initialization failed for '+ip+' '+action+': '+reason)
        self.ip, self.action, self.reason = ip, action, reason


def request(ip, action='status.get', params=None, timeout=5, capture=None):
    if action not in ACTIONS:
        raise ValueError('Only allowlisted read actions are permitted')
    c = http.client.HTTPConnection(ip, timeout=timeout)
    start = time.monotonic()
    try:
        c.request('POST', '/api/v1/command', json.dumps({'action': action, 'params': params or {}}), {'Content-Type': 'application/json'})
        r = c.getresponse()
        if capture is not None:
            capture['http'] = r.status
        raw = r.read()
        if capture is not None:
            capture['raw'] = raw
        try:
            reply = json.loads(raw)
            if not isinstance(reply, dict) or not isinstance(reply.get('code'), str):
                raise ValueError('Missing command envelope')
            if r.status == 200 and reply.get('code') == 'ok' and not isinstance(reply.get('data'), dict):
                raise ValueError('Missing command data')
        except ValueError:
            if r.status == 200:
                raise ProtocolError(r.status, raw)
            reply = {}
        return r.status, reply, len(raw), (time.monotonic()-start)*1000, start
    finally:
        c.close()


class Runner:
    def __init__(self, targets, version, output, versions=None, plan=None,
                 require_shared_core=False):
        self.targets, self.version, self.output = targets, version, output
        self.versions = versions or {}
        self.plan = plan or {}
        self.plan_completion_required = plan is not None
        self.require_shared_core = require_shared_core
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.rows, self.blockers, self.before, self.memory = [], [], {}, []
        self.phases, self.recoveries, self.scenarios = [], [], []
        self.completion_checks = {
            'network_restore': False,
            'configuration_unchanged': False,
            'idle_quiescence': False,
        }
        self.run = {
            'run_id': uuid.uuid4().hex,
            'started_at': time.time(),
            'status': 'running',
            'completed': False,
            'plan': self.plan,
            'plan_completion_required': self.plan_completion_required,
            'require_shared_core': require_shared_core,
        }
        output.mkdir(parents=True, exist_ok=True)

    def block(self, ip, reason):
        with self.lock:
            self.blockers.append({'ip': ip, 'reason': reason, 'at': time.time()})
        self.stop.set()
        print('BLOCKER', ip, reason, flush=True)

    def check(self, ip, state, start):
        ref = self.before[ip]
        if state.get('device_id', '').lower() != ref['id'].lower() or state.get('version') != self.versions.get(ip, self.version):
            self.block(ip, 'identity/version changed')
        uptime = state.get('uptime_seconds')
        if uptime is None or start-uptime > ref['boot_origin']+5:
            self.block(ip, 'unexpected uptime reset or missing uptime')
        model = ref['state'].get('model')
        for key in CONTROL_KEYS_BY_MODEL.get(model, ()):
            if key not in ref['state'] or key not in state:
                self.block(ip, 'missing control state field: '+key)
            elif state[key] != ref['state'][key]:
                self.block(ip, 'control state changed: '+key)
        if state.get('watchdog_state') not in (None, 'healthy', 'starting'):
            self.block(ip, 'unhealthy watchdog')

    def sample(self, ip, phase, action='status.get', params=None, expected='ok'):
        row = {'ip': ip, 'phase': phase, 'action': action, 'params': params or {}, 'at': time.time()}
        try:
            status, reply, size, elapsed, start = request(ip, action, params)
            row.update(http=status, code=reply.get('code'), bytes=size, ms=elapsed)
            row['ok'] = status == 200 and reply.get('code') == expected
            if status == 200 and reply.get('code') not in (expected, 'busy'):
                self.block(ip, 'unexpected command result: '+str(reply.get('code')))
            elif status not in (200, 503):
                self.block(ip, 'unexpected HTTP status: '+str(status))
            if row['ok'] and action == 'status.get' and expected == 'ok':
                state = reply['data']
                row.update(uptime=state.get('uptime_seconds'), free_memory=state.get('free_memory'),
                           wifi_connected=state.get('wifi_connected'), mqtt_connected=state.get('mqtt_connected'))
                self.check(ip, state, start)
        except ProtocolError as e:
            row.update(ok=False, http=e.status, error=str(e), raw_base64=base64.b64encode(e.raw).decode())
            self.block(ip, 'malformed successful HTTP response')
        except (OSError, ValueError, http.client.HTTPException) as e:
            row.update(ok=False, error=type(e).__name__+': '+str(e))
        with self.lock:
            self.rows.append(row)
        return row

    def initialize(self):
        for ip, ident in self.targets.items():
            status, reply, _, _, start, capture = self._initialize_request(ip, 'status.get')
            if status != 200 or reply.get('code') != 'ok':
                self._fail_initialize(ip, 'status.get', status, reply,
                                       'baseline status request was not successful', capture)
            state = reply['data']
            if (not isinstance(state.get('device_id'), str) or
                    state['device_id'].lower() != ident.lower() or
                    state.get('version') != self.versions.get(ip, self.version)):
                self._fail_initialize(ip, 'status.get', status, reply,
                                       'baseline identity or version mismatch', capture)
            if state.get('watchdog_state') not in (None, 'healthy'):
                self._fail_initialize(ip, 'status.get', status, reply,
                                       'baseline watchdog is unhealthy', capture)
            required = CONTROL_KEYS_BY_MODEL.get(state.get('model'), ())
            if not required or any(key not in state for key in required):
                self._fail_initialize(ip, 'status.get', status, reply,
                                       'baseline status missing required control fields', capture)
            if not isinstance(state.get('uptime_seconds'), (int, float)):
                self._fail_initialize(ip, 'status.get', status, reply,
                                       'baseline status missing uptime', capture)

            cfg_status, cfg, _, _, _, capture = self._initialize_request(ip, 'config.get')
            if cfg_status != 200 or cfg.get('code') != 'ok':
                self._fail_initialize(ip, 'config.get', cfg_status, cfg,
                                       'baseline configuration request was not successful', capture)
            self.before[ip] = {'id': ident, 'state': state, 'config': cfg['data'],
                               'boot_origin': start-state['uptime_seconds']}
        self.save()
        self.memory_window('baseline')
        if not self.stop.is_set() and not self.wait_idle_quiescence('baseline'):
            self.block('', 'baseline idle state did not reach three zero-allocation samples')

    def _initialize_request(self, ip, action):
        capture = {}
        try:
            result = request(ip, action, capture=capture)
        except ProtocolError as exc:
            capture.update(http=exc.status, raw=exc.raw)
            self._fail_initialize(ip, action, exc.status, {}, str(exc), capture)
        except (OSError, ValueError, http.client.HTTPException) as exc:
            self._fail_initialize(ip, action, capture.get('http'), {},
                                   type(exc).__name__+': '+str(exc), capture)
        status, reply, size, elapsed, start = result
        capture.setdefault('http', status)
        return status, reply, size, elapsed, start, capture

    def _fail_initialize(self, ip, action, status, reply, reason, capture=None):
        capture = capture or {}
        raw = capture.get('raw')
        row = {
            'ip': ip,
            'phase': 'initialize',
            'action': action,
            'params': {},
            'at': time.time(),
            'ok': False,
            'http': status,
            'code': reply.get('code') if isinstance(reply, dict) else None,
            'error': reason,
        }
        if raw is not None:
            row['bytes'] = len(raw)
            row['raw_base64'] = base64.b64encode(raw).decode()
        self.rows.append(row)
        self.block(ip, 'initialization failed: '+reason)
        self.run.update(
            status='failed', completed=False, ended_at=time.time(),
            failure_type='InitializationError', failure_reason=reason,
        )
        self.save()
        raise InitializationError(ip, action, reason)

    def memory_sample(self, ip, phase, timeout=5):
        c = http.client.HTTPConnection(ip, timeout=timeout)
        row = {'ip': ip, 'phase': phase, 'at': time.time()}
        try:
            c.request('GET', '/api/v1/memory')
            r = c.getresponse()
            reply = json.loads(r.read())
            row.update(http=r.status, ok=r.status == 200 and reply.get('code') == 'ok')
            if row['ok']:
                row['data'] = reply['data']
                if self.require_shared_core:
                    self.check_shared_core(ip, row['data'])
        except (OSError, ValueError, http.client.HTTPException) as e:
            row.update(ok=False, error=str(e))
        finally:
            c.close()
        self.memory.append(row)

    def check_shared_core(self, ip, data):
        """Require actual owner diagnostics, without inferring SDK heap bounds."""
        if not isinstance(data, dict):
            self.block(ip, 'missing shared HTTP diagnostics')
            return
        required = ('http_core_owner_state', 'http_core_close_failures',
                    'http_core_restarts', 'http_core_tcp_limit',
                    'http_core_tcp_rc', 'http_core_occupied',
                    'http_core_server_bytes', 'http_tcp_sample_ms')
        if any(type(data.get(key)) is not int for key in required):
            self.block(ip, 'missing or invalid shared HTTP diagnostics')
            return
        if data['http_core_owner_state'] != 1 or data['http_core_close_failures'] != 0:
            self.block(ip, 'shared HTTP owner unavailable or uncertain close')
        if data['http_core_restarts'] != 0:
            self.block(ip, 'shared HTTP owner restarted; investigate before acceptance')
        model = self.before.get(ip, {}).get('state', {}).get('model')
        expected = {'a1': 3, 'tc1': 3, 'm1': 4}.get(model)
        if (expected is None or data['http_core_tcp_limit'] != expected or
                data['http_core_tcp_rc'] != 0 or
                not 0 <= data['http_core_occupied'] <= expected or
                data['http_core_server_bytes'] <= 0):
            self.block(ip, 'shared HTTP admission diagnostics invalid')

    def memory_window(self, phase):
        print('MEMORY idle window', phase, flush=True)
        for _ in range(10):
            if self.stop.is_set():
                return
            for ip in self.targets:
                self.memory_sample(ip, phase)
            self.stop.wait(1)
        self.save()

    def idle_zero_tail(self, ip, phase, required=3):
        streak = 0
        for row in reversed(self.memory):
            if row.get('ip') != ip or row.get('phase') != phase:
                continue
            data = row.get('data') if row.get('ok') else None
            zero = isinstance(data, dict) and all(
                data.get(key) == 0 for key in
                ('json_live_blocks', 'memory_active', 'memory_reserved', 'memory_waiting'))
            if not zero:
                return False
            streak += 1
            if streak >= required:
                return True
        return False

    def wait_idle_quiescence(self, phase, extra_seconds=10):
        deadline = time.monotonic() + extra_seconds
        while not self.stop.is_set():
            pending = [ip for ip in self.targets if not self.idle_zero_tail(ip, phase)]
            if not pending:
                return True
            if time.monotonic() >= deadline:
                return False
            for ip in pending:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self.memory_sample(ip, phase, timeout=min(5, remaining))
            self.save()
            self.stop.wait(min(1, max(0, deadline-time.monotonic())))
        return False

    def scenario(self, name, action):
        record = {
            'name': name,
            'started_at': time.time(),
            'started_monotonic': time.monotonic(),
            'completed': False,
        }
        self.scenarios.append(record)
        self.save()
        try:
            action()
            record['completed'] = not self.stop.is_set()
        except BaseException as exc:
            record['failure_type'] = type(exc).__name__
            raise
        finally:
            record['ended_at'] = time.time()
            record['elapsed_seconds'] = time.monotonic() - record['started_monotonic']
            record.pop('started_monotonic', None)
            self.save()

    def save_interrupted(self, exc):
        if self.run['status'] == 'running':
            self.run.update(
                status='interrupted', completed=False, ended_at=time.time(),
                failure_type=type(exc).__name__,
            )
        try:
            self.save()
        except Exception as save_exc:
            print('EVIDENCE SAVE FAILED', type(save_exc).__name__, flush=True)

    def _planned_steps_complete(self):
        if not self.plan_completion_required:
            return True
        steps = self.plan.get('steps', [])
        if not isinstance(steps, list) or not steps:
            return False
        if not all(
            len([record for record in self.scenarios
                 if record.get('name') == step and record.get('completed') is True]) == 1
            for step in steps
        ):
            return False
        for planned in self.plan.get('phases', []):
            matches = [record for record in self.phases
                       if record.get('name') == planned.get('name')]
            if len(matches) != 1 or matches[0].get('completed') is not True:
                return False
            elapsed = matches[0].get('elapsed_seconds')
            requested = planned.get('requested_seconds')
            clients_match = (matches[0].get('clients_per_device') ==
                             planned.get('clients_per_device'))
            if (not isinstance(elapsed, (int, float)) or elapsed < requested or
                    matches[0].get('requested_seconds') != requested or
                    not clients_match):
                return False
        recovery_steps = self.plan.get('recovery_steps', [])
        if not isinstance(recovery_steps, list):
            return False
        target_ips = set(self.targets)
        for planned in recovery_steps:
            if not isinstance(planned, dict):
                return False
            name = planned.get('name')
            expected_targets = set(planned.get('targets', []))
            if not name or not expected_targets or not expected_targets <= target_ips:
                return False
            matches = [record for record in self.recoveries
                       if record.get('name') == name]
            covered = set()
            for record in matches:
                record_targets = record.get('targets', {})
                if record.get('completed') is not True or not isinstance(record_targets, dict):
                    return False
                for ip, result in record_targets.items():
                    if ip not in expected_targets or ip in covered:
                        return False
                    if not isinstance(result, dict) or result.get('completed') is not True:
                        return False
                    covered.add(ip)
            if covered != expected_targets:
                return False
        return True

    def worker(self, ip, phase, deadline, number, period, offset):
        index = number
        self.stop.wait(offset)
        while time.monotonic() < deadline and not self.stop.is_set():
            index += 1
            mode = index % 10
            if mode < 6:
                self.sample(ip, phase, params={'view': 'live'})
            elif mode == 6:
                self.sample(ip, phase)
            elif mode == 7:
                self.sample(ip, phase, 'capabilities.get')
            elif mode == 8:
                if (index // 10) % 2:
                    action = 'power.get' if self.before[ip]['state']['model'] == 'tc1' else 'binding.status.get'
                    self.sample(ip, phase, action)
                else:
                    self.sample(ip, phase, 'logs.get', {'limit': 8})
            else:
                self.sample(ip, phase, params={'view': 'invalid-read-only-view'}, expected='invalid_params')
            self.stop.wait(period)

    def recover(self, phase, seconds=45):
        record = {
            'name': phase,
            'started_at': time.time(),
            'started_monotonic': time.monotonic(),
            'required_consecutive_successes': 3,
            'targets': {},
            'completed': False,
        }
        self.recoveries.append(record)
        try:
            for ip in self.targets:
                deadline, good, attempts = time.monotonic()+seconds, 0, 0
                try:
                    while time.monotonic() < deadline and not self.stop.is_set():
                        row = self.sample(ip, phase)
                        attempts += 1
                        good = good+1 if row['ok'] else 0
                        if good >= 3:
                            break
                        self.stop.wait(1)
                finally:
                    record['targets'][ip] = {
                        'attempts': attempts,
                        'consecutive_successes': good,
                        'completed': good >= 3,
                    }
                if good < 3 and not self.stop.is_set():
                    self.block(ip, 'no sustained recovery after load removed')
        finally:
            record['completed'] = (not self.stop.is_set() and
                                   all(v['completed'] for v in record['targets'].values()) and
                                   set(record['targets']) == set(self.targets))
            record['ended_at'] = time.time()
            record['elapsed_seconds'] = time.monotonic() - record.pop('started_monotonic')
            self.save()

    def phase(self, name, concurrency, seconds, period=.2):
        print('START', name, 'clients/device', concurrency, 'seconds', seconds, flush=True)
        started = time.monotonic()
        deadline = started+seconds
        last_report = time.monotonic()
        phase_record = {
            'name': name,
            'clients_per_device': concurrency,
            'requested_seconds': seconds,
            'started_at': time.time(),
            'completed': False,
        }
        self.phases.append(phase_record)
        self.save()
        normal_exit = False
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency*len(self.targets)) as pool:
                jobs = [pool.submit(self.worker, ip, name, deadline, i, period,
                                    i*period/concurrency if name == 'soak' else 0)
                        for ip in self.targets for i in range(concurrency)]
                while any(not job.done() for job in jobs):
                    self.stop.wait(1)
                    if time.monotonic()-last_report >= 60:
                        for ip in self.targets:
                            self.memory_sample(ip, name)
                        self.save()
                        print('PROGRESS', name, 'remaining', max(0, round(deadline-time.monotonic())), 'requests', len(self.rows), flush=True)
                        last_report = time.monotonic()
                    if self.stop.is_set():
                        break
                for job in jobs:
                    job.result()
            normal_exit = True
        finally:
            elapsed = time.monotonic() - started
            phase_record.update(
                ended_at=time.time(), elapsed_seconds=elapsed,
                completed=(normal_exit and not self.stop.is_set() and elapsed >= seconds),
            )
            if normal_exit and not phase_record['completed'] and not self.stop.is_set():
                self.block('', 'phase ended before requested duration: '+name)
            self.save()
        self.recover(name+'-recovery')
        print('END', name, 'blockers', len(self.blockers), flush=True)

    def slow_clients(self):
        print('START incomplete read headers', flush=True)
        for ip in self.targets:
            if self.stop.is_set():
                break
            sockets = []
            try:
                for _ in range(8):
                    try:
                        sock = socket.create_connection((ip, 80), timeout=3)
                        sock.sendall(('GET /api/v1/status HTTP/1.1\r\nHost: '+ip+'\r\n').encode())
                        sockets.append(sock)
                    except OSError:
                        pass
                self.stop.wait(3)
            finally:
                for sock in sockets:
                    sock.close()
            self.recover('incomplete-headers-recovery')

    def aborted_clients(self):
        print('START reset/half-closed read connections', flush=True)
        for ip in self.targets:
            for index in range(48):
                if self.stop.is_set():
                    return
                try:
                    with socket.create_connection((ip, 80), timeout=3) as sock:
                        body = b'{"action":"status.get","params":{"view":"live"}}'
                        header = ('POST /api/v1/command HTTP/1.1\r\nHost: '+ip+
                                  '\r\nContent-Type: application/json\r\nContent-Length: '+str(len(body))+'\r\n\r\n').encode()
                        sock.sendall(header+(body if index % 3 else body[:8]))
                        if index % 2:
                            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                        else:
                            sock.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
                self.stop.wait(.1)
            self.recover('aborted-connections-recovery')

    def buffered_read_clients(self):
        """Bound pending TCP input using only a padded status.get request."""
        print('START buffered 8192-byte read requests', flush=True)
        body = b'{"action":"status.get","params":{"view":"live"}}'
        body += b' ' * (8192-len(body))

        def client(ip):
            try:
                with socket.create_connection((ip, 80), timeout=3) as sock:
                    header = ('POST /api/v1/command HTTP/1.1\r\nHost: '+ip+
                              '\r\nContent-Type: application/json\r\nContent-Length: 8192\r\n\r\n').encode()
                    sock.sendall(header+body[:4096])
                    self.stop.wait(3)
                    if not self.stop.is_set():
                        sock.sendall(body[4096:])
                        # Content-Length-aware reading is covered by normal
                        # requests; this case bounds pending input and recovery.
                        sock.shutdown(socket.SHUT_WR)
                        sock.recv(1024)
            except OSError:
                pass

        for ip in self.targets:
            if self.stop.is_set():
                return
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                jobs = [pool.submit(client, ip) for _ in range(8)]
                self.stop.wait(2)
                self.memory_sample(ip, 'buffered-read-input')
                self.save()
                for job in jobs:
                    job.result()
            self.recover('buffered-read-input-recovery')

    def missing_read_clients(self):
        """Exercise SDK cleanup when an unknown GET body ends at EOF."""
        print('START missing read resource with truncated body EOF', flush=True)
        for ip in self.targets:
            if self.stop.is_set():
                return
            for size in (0, 4, 8):
                try:
                    with socket.create_connection((ip, 80), timeout=5) as sock:
                        header = ('GET /__readonly_missing_resource__ HTTP/1.1\r\nHost: '+ip+
                                  '\r\nContent-Length: 8\r\nConnection: close\r\n\r\n').encode()
                        sock.sendall(header+b' '*size)
                        sock.shutdown(socket.SHUT_WR)
                        sock.recv(1024)
                except OSError:
                    pass
            self.recover('missing-read-eof-recovery')

    def save(self):
        with self.lock:
            rows, blockers = self.rows[:], self.blockers[:]
            memory = self.memory[:]
        groups = {}
        for ip in self.targets:
            for phase in sorted({r['phase'] for r in rows if r['ip'] == ip}):
                items = [r for r in rows if r['ip'] == ip and r['phase'] == phase]
                good = [r for r in items if r['ok']]
                times = sorted(r['ms'] for r in good)
                groups[ip+' '+phase] = {'requests': len(items), 'success': len(good), 'degraded': len(items)-len(good),
                    'p50_ms': statistics.median(times) if times else None,
                    'p95_ms': times[math.ceil(len(times)*.95)-1] if times else None}
        data = {
            'version': self.version,
            'target_versions': self.versions,
            'run': self.run,
            'phases': self.phases,
            'recoveries': self.recoveries,
            'scenarios': self.scenarios,
            'completion_checks': self.completion_checks,
            'blockers': blockers,
            'summary': groups,
            'rows': rows,
            'before': self.before,
            'memory': memory,
        }
        p = self.output/'evidence.json'
        p.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n')
        p.chmod(0o600)

    def restore_connectivity(self):
        restored = True
        for ip in self.targets:
            required = [key for key in ('wifi_connected', 'mqtt_connected') if self.before[ip]['state'].get(key) is True]
            if not required or self.stop.is_set():
                continue
            deadline, good = time.monotonic()+90, False
            while time.monotonic() < deadline and not self.stop.is_set():
                row = self.sample(ip, 'final-connectivity-recovery')
                if row['ok'] and all(row.get(key) is True for key in required):
                    good = True
                    break
                self.stop.wait(1)
            if not good and not self.stop.is_set():
                self.block(ip, 'initial network readiness did not recover after load removed')
                restored = False
            if not good:
                restored = False
        self.completion_checks['network_restore'] = restored and not self.stop.is_set()
        return self.completion_checks['network_restore']

    def finish(self):
        network_ok = self.restore_connectivity()
        idle_ok = False
        if not self.stop.is_set():
            self.memory_window('final')
            idle_ok = self.wait_idle_quiescence('final')
            if not idle_ok and not self.stop.is_set():
                self.block('', 'final idle state did not reach three zero-allocation samples')
            for ip in self.targets:
                windows = {p: [r['data'] for r in self.memory if r['ip'] == ip and r['phase'] == p and r['ok']] for p in ('baseline', 'final')}
                if any(len(windows[p]) < 8 for p in windows):
                    self.block(ip, 'insufficient idle memory evidence')
                    continue
                for phase, values in windows.items():
                    if not self.idle_zero_tail(ip, phase):
                        self.block(ip, 'idle window lacks three terminal zero-allocation samples: '+phase)
                initial_values = [v['free_memory'] for v in windows['baseline']
                                  if isinstance(v.get('free_memory'), (int, float))]
                final_values = [v['free_memory'] for v in windows['final']
                                if isinstance(v.get('free_memory'), (int, float))]
                if not initial_values or not final_values:
                    self.block(ip, 'missing heap samples for baseline/final comparison')
                elif statistics.median(initial_values)-statistics.median(final_values) > 1024:
                    self.block(ip, 'unexplained persistent heap loss greater than 1024 bytes; investigate')
        self.completion_checks['idle_quiescence'] = idle_ok and not self.stop.is_set()
        config_ok = True
        if not self.stop.is_set():
            for ip in self.targets:
                try:
                    status, reply, *_ = request(ip, 'config.get')
                except (OSError, ValueError, http.client.HTTPException):
                    status, reply = None, {}
                if status != 200 or reply.get('code') != 'ok' or reply.get('data') != self.before[ip]['config']:
                    self.block(ip, 'configuration changed or unavailable')
                    config_ok = False
                    break
        else:
            config_ok = False
        self.completion_checks['configuration_unchanged'] = config_ok
        complete = (not self.stop.is_set() and not self.blockers and network_ok and
                    idle_ok and config_ok and self._planned_steps_complete())
        self.run.update(
            status='complete' if complete else 'failed',
            completed=complete,
            ended_at=time.time(),
            failure_type=None if complete else ('Blocker' if self.blockers else 'IncompletePlan'),
        )
        self.save()
        if not complete:
            raise SystemExit(1)
        print('PASS requested plan completed with recovery and final gates', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--target', action='append', required=True, help='IP=expected_device_id')
    ap.add_argument('--version', required=True)
    ap.add_argument('--target-version', action='append', default=[],
                    help='Explicit IP=version override for a mixed-candidate comparison')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--soak-seconds', type=int, default=1800)
    ap.add_argument('--phase-seconds', type=int, default=60)
    ap.add_argument('--normal-phase-seconds', type=int,
                    help='Optional longer 1/4-client phases, seconds')
    ap.add_argument('--soak-clients', type=int, choices=(1, 4), default=1)
    ap.add_argument('--soak-period', type=float, default=.2,
                    help='Minimum delay after each worker request, seconds')
    ap.add_argument('--soak-only', action='store_true',
                    help='Additional stability stage after the same candidate passed load/recovery')
    ap.add_argument('--require-shared-core', action='store_true',
                    help='Require healthy shared owner and verified model admission diagnostics')
    a = ap.parse_args()
    if not 60 <= a.soak_seconds <= 14400 or not 10 <= a.phase_seconds <= 300:
        ap.error('soak 60..14400 seconds; phase 10..300 seconds')
    if a.normal_phase_seconds is not None and not 10 <= a.normal_phase_seconds <= 600:
        ap.error('normal phase 10..600 seconds')
    if not math.isfinite(a.soak_period) or not .2 <= a.soak_period <= 10:
        ap.error('soak period .2..10 seconds')
    targets = dict(v.split('=', 1) for v in a.target)
    if not targets or len(targets) != len(a.target):
        ap.error('Use unique explicit IP=identity targets')
    versions = dict(v.split('=', 1) for v in a.target_version)
    if len(versions) != len(a.target_version) or not versions.keys() <= targets.keys():
        ap.error('Version overrides must name unique explicit targets')
    planned_steps = []
    planned_phases = []
    if not a.soak_only:
        for clients in (1, 4, 8, 16):
            seconds = (a.normal_phase_seconds
                       if clients <= 4 and a.normal_phase_seconds is not None
                       else a.phase_seconds)
            name = 'concurrency-'+str(clients)
            planned_steps.append(name)
            planned_phases.append({
                'name': name,
                'clients_per_device': clients,
                'requested_seconds': seconds,
            })
        planned_steps.extend((
            'slow-clients', 'aborted-clients', 'buffered-read-clients',
            'missing-read-clients',
        ))
    planned_steps.append('soak')
    planned_phases.append({
        'name': 'soak',
        'clients_per_device': a.soak_clients,
        'requested_seconds': a.soak_seconds,
    })
    plan = {
        'mode': 'soak-only' if a.soak_only else 'full',
        'targets': [
            {'ip': ip, 'device_id': ident,
             'version': versions.get(ip, a.version)}
            for ip, ident in targets.items()
        ],
        'steps': planned_steps,
        'phases': planned_phases,
        'phase_seconds': a.phase_seconds,
        'normal_phase_seconds': a.normal_phase_seconds,
        'soak_seconds': a.soak_seconds,
        'soak_clients': a.soak_clients,
        'soak_period': a.soak_period,
    }
    plan['recovery_steps'] = [
        {'name': phase['name']+'-recovery', 'targets': list(targets)}
        for phase in planned_phases]
    if not a.soak_only:
        plan['recovery_steps'].extend((
            {'name': name, 'targets': list(targets)}
            for name in ('incomplete-headers-recovery',
                         'aborted-connections-recovery',
                         'buffered-read-input-recovery',
                         'missing-read-eof-recovery')
        ))
    with open('/tmp/phicomm-readonly-stress.lock', 'w') as guard:
        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        runner = Runner(targets, a.version, a.output, versions, plan,
                        require_shared_core=a.require_shared_core)

        def interrupted(signum, _frame):
            runner.stop.set()
            raise KeyboardInterrupt('received signal '+str(signum))

        old_sigint = signal.signal(signal.SIGINT, interrupted)
        old_sigterm = signal.signal(signal.SIGTERM, interrupted)
        try:
            runner.initialize()
            if not a.soak_only:
                for clients in (1, 4, 8, 16):
                    if runner.stop.is_set():
                        break
                    seconds = (a.normal_phase_seconds
                               if clients <= 4 and a.normal_phase_seconds is not None
                               else a.phase_seconds)
                    name = 'concurrency-'+str(clients)
                    runner.scenario(name, lambda n=name, c=clients, d=seconds:
                                    runner.phase(n, c, d))
                if not runner.stop.is_set():
                    runner.scenario('slow-clients', runner.slow_clients)
                if not runner.stop.is_set():
                    runner.scenario('aborted-clients', runner.aborted_clients)
                if not runner.stop.is_set():
                    runner.scenario('buffered-read-clients', runner.buffered_read_clients)
                if not runner.stop.is_set():
                    runner.scenario('missing-read-clients', runner.missing_read_clients)
            if not runner.stop.is_set():
                runner.scenario('soak', lambda: runner.phase(
                    'soak', a.soak_clients, a.soak_seconds, a.soak_period))
            runner.finish()
        except BaseException as exc:
            runner.save_interrupted(exc)
            raise
        finally:
            signal.signal(signal.SIGINT, old_sigint)
            signal.signal(signal.SIGTERM, old_sigterm)


if __name__ == '__main__':
    main()
