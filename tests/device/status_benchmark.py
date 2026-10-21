#!/usr/bin/env python3
"""Paced, read-only status comparison for one explicitly identified device."""
import argparse
import json
import math
import pathlib
import statistics
import time
import urllib.request


def read(ip, action, params=None):
    if action not in ('status.get', 'config.get', 'capabilities.get', 'power.get'):
        raise ValueError('Read-only action required')
    payload = json.dumps({'action': action, 'params': params or {}}).encode()
    request = urllib.request.Request('http://' + ip + '/api/v1/command', payload,
                                     {'Content-Type': 'application/json'})
    start = time.perf_counter()
    with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=8) as response:
        raw = response.read()
    elapsed = (time.perf_counter() - start) * 1000
    reply = json.loads(raw)
    if reply.get('code') != 'ok':
        raise RuntimeError(reply.get('code'))
    return reply['data'], len(raw), elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ip', required=True)
    parser.add_argument('--device-id', required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--view', choices=('default', 'full', 'live'), default='default')
    parser.add_argument('--samples', type=int, default=30)
    parser.add_argument('--period', type=float, default=1)
    args = parser.parse_args()
    if not 1 <= args.samples <= 120 or args.period < 1:
        parser.error('Use 1-120 samples with period >= 1 second')
    before, _, _ = read(args.ip, 'status.get')
    assert before['device_id'].lower() == args.device_id.lower(), 'Wrong device'
    config, _, _ = read(args.ip, 'config.get')
    capabilities, _, _ = read(args.ip, 'capabilities.get')
    rows = []
    for _ in range(args.samples):
        start = time.monotonic()
        try:
            state, size, elapsed = read(args.ip, 'status.get', {} if args.view == 'default' else {'view': args.view})
            assert state['device_id'].lower() == args.device_id.lower()
            assert state.get('socket_states') == before.get('socket_states'), 'Outlet state changed; stop'
            rows.append({'ok': True, 'bytes': size, 'ms': elapsed, 'uptime': state.get('uptime_seconds'),
                         'free_memory': state.get('free_memory')})
        except (OSError, RuntimeError) as error:
            rows.append({'ok': False, 'error': str(error)})
        time.sleep(max(0, args.period - (time.monotonic() - start)))
    after, _, _ = read(args.ip, 'status.get')
    assert after.get('socket_states') == before.get('socket_states')
    good = [row for row in rows if row['ok']]
    times = sorted(row['ms'] for row in good)
    summary = {'view': args.view, 'samples': len(rows), 'success': len(good),
               'mean_bytes': statistics.mean(row['bytes'] for row in good) if good else None,
               'p50_ms': statistics.median(times) if times else None,
               'p95_ms': times[math.ceil(len(times) * .95)-1] if times else None,
               'before_free_memory': before.get('free_memory'), 'after_free_memory': after.get('free_memory')}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'summary': summary, 'rows': rows, 'before': before,
        'after': after, 'config': config, 'capabilities': capabilities}, indent=2, ensure_ascii=False)+'\n')
    args.output.chmod(0o600)
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
