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
import socket
import statistics
import struct
import threading
import time

ACTIONS = ('status.get', 'capabilities.get', 'logs.get', 'config.get',
           'power.get', 'binding.status.get')


class ProtocolError(ValueError):
    def __init__(self, status, raw):
        super().__init__("Malformed successful HTTP command response")
        self.status, self.raw = status, raw


def request(ip, action='status.get', params=None, timeout=5):
    if action not in ACTIONS:
        raise ValueError('Only allowlisted read actions are permitted')
    c = http.client.HTTPConnection(ip, timeout=timeout)
    start = time.monotonic()
    try:
        c.request('POST', '/api/v1/command', json.dumps({'action': action, 'params': params or {}}), {'Content-Type': 'application/json'})
        r = c.getresponse()
        raw = r.read()
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
    def __init__(self, targets, version, output, versions=None):
        self.targets, self.version, self.output = targets, version, output
        self.versions = versions or {}
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.rows, self.blockers, self.before, self.memory = [], [], {}, []
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
        for key in ('socket_states', 'requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled', 'brightness_requested'):
            if key in state and key in ref['state'] and state[key] != ref['state'][key]:
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
            status, reply, _, _, start = request(ip)
            assert status == 200 and reply['code'] == 'ok'
            state = reply['data']
            assert state['device_id'].lower() == ident.lower() and state['version'] == self.versions.get(ip, self.version)
            assert state.get('watchdog_state') in (None, 'healthy')
            status, cfg, *_ = request(ip, 'config.get')
            assert status == 200 and cfg['code'] == 'ok'
            self.before[ip] = {'id': ident, 'state': state, 'config': cfg['data'], 'boot_origin': start-state['uptime_seconds']}
        self.save()
        self.memory_window('baseline')

    def memory_sample(self, ip, phase):
        c = http.client.HTTPConnection(ip, timeout=5)
        row = {'ip': ip, 'phase': phase, 'at': time.time()}
        try:
            c.request('GET', '/api/v1/memory')
            r = c.getresponse()
            reply = json.loads(r.read())
            row.update(http=r.status, ok=r.status == 200 and reply.get('code') == 'ok')
            if row['ok']:
                row['data'] = reply['data']
        except (OSError, ValueError, http.client.HTTPException) as e:
            row.update(ok=False, error=str(e))
        finally:
            c.close()
        self.memory.append(row)

    def memory_window(self, phase):
        print('MEMORY idle window', phase, flush=True)
        for _ in range(10):
            if self.stop.is_set():
                return
            for ip in self.targets:
                self.memory_sample(ip, phase)
            self.stop.wait(1)
        self.save()

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
        for ip in self.targets:
            deadline, good = time.monotonic()+seconds, 0
            while time.monotonic() < deadline and not self.stop.is_set():
                row = self.sample(ip, phase)
                good = good+1 if row['ok'] else 0
                if good >= 3:
                    break
                self.stop.wait(1)
            if good < 3 and not self.stop.is_set():
                self.block(ip, 'no sustained recovery after load removed')
        self.save()

    def phase(self, name, concurrency, seconds, period=.2):
        print('START', name, 'clients/device', concurrency, 'seconds', seconds, flush=True)
        deadline = time.monotonic()+seconds
        last_report = time.monotonic()
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
        groups = {}
        for ip in self.targets:
            for phase in sorted({r['phase'] for r in rows if r['ip'] == ip}):
                items = [r for r in rows if r['ip'] == ip and r['phase'] == phase]
                good = [r for r in items if r['ok']]
                times = sorted(r['ms'] for r in good)
                groups[ip+' '+phase] = {'requests': len(items), 'success': len(good), 'degraded': len(items)-len(good),
                    'p50_ms': statistics.median(times) if times else None,
                    'p95_ms': times[math.ceil(len(times)*.95)-1] if times else None}
        data = {'version': self.version, 'target_versions': self.versions, 'blockers': blockers, 'summary': groups, 'rows': rows, 'before': self.before, 'memory': self.memory}
        p = self.output/'evidence.json'
        p.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n')
        p.chmod(0o600)

    def restore_connectivity(self):
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

    def finish(self):
        self.restore_connectivity()
        if not self.stop.is_set():
            self.memory_window('final')
            for ip in self.targets:
                windows = {p: [r['data'] for r in self.memory if r['ip'] == ip and r['phase'] == p and r['ok']] for p in ('baseline', 'final')}
                if any(len(windows[p]) < 8 for p in windows):
                    self.block(ip, 'insufficient idle memory evidence')
                    continue
                for phase, values in windows.items():
                    if not any(v.get('json_live_blocks') == 0 and v.get('memory_active') == 0 and v.get('memory_reserved') == 0 for v in values):
                        self.block(ip, 'outstanding JSON/admission allocations during idle '+phase)
                initial = statistics.median(v['free_memory'] for v in windows['baseline'])
                final = statistics.median(v['free_memory'] for v in windows['final'])
                if initial-final > 1024:
                    self.block(ip, 'unexplained persistent heap loss greater than 1024 bytes; investigate')
        for ip in self.targets:
            if self.stop.is_set():
                break
            status, reply, *_ = request(ip, 'config.get')
            if status != 200 or reply.get('data') != self.before[ip]['config']:
                self.block(ip, 'configuration changed or unavailable')
        self.save()
        if self.blockers:
            raise SystemExit(1)
        print('PASS no reset/deadlock/control/config changes in completed scenarios', flush=True)


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
    with open('/tmp/phicomm-readonly-stress.lock', 'w') as guard:
        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        runner = Runner(targets, a.version, a.output, versions)
        runner.initialize()
        if not a.soak_only:
            for clients in (1, 4, 8, 16):
                if runner.stop.is_set():
                    break
                seconds = a.normal_phase_seconds if clients <= 4 and a.normal_phase_seconds is not None else a.phase_seconds
                runner.phase('concurrency-'+str(clients), clients, seconds)
            if not runner.stop.is_set():
                runner.slow_clients()
            if not runner.stop.is_set():
                runner.aborted_clients()
            if not runner.stop.is_set():
                runner.buffered_read_clients()
            if not runner.stop.is_set():
                runner.missing_read_clients()
        if not runner.stop.is_set():
            runner.phase('soak', a.soak_clients, a.soak_seconds, a.soak_period)
        runner.finish()


if __name__ == '__main__':
    main()
