#!/usr/bin/env python3
"""Read-only SCD40 acceptance: identity, CRC/errors, freshness and sample progress."""
import argparse
import json
import time
import urllib.request
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ip', required=True)
    parser.add_argument('--device-id', required=True)
    parser.add_argument('--seconds', type=int, default=65)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    rows = []
    deadline = time.monotonic() + args.seconds
    with args.output.open('w') as output:
        while time.monotonic() < deadline:
            with http.open(f'http://{args.ip}/api/v1/status', timeout=10) as response:
                envelope = json.load(response)
            status = envelope['data']
            assert status['device_id'] == args.device_id, 'Wrong device'
            row = {key: status.get(key) for key in (
                'device_id', 'version', 'uptime_ms', 'uart_ready', 'sensor_valid',
                'uart_errors', 'mqtt_connected', 'watchdog_state', 'scd40')}
            row['observed_at'] = time.time()
            rows.append(row)
            output.write(json.dumps(row, ensure_ascii=False) + '\n')
            output.flush()
            print(json.dumps(row, ensure_ascii=False), flush=True)
            time.sleep(5)
    assert len(rows) >= 3, 'Need at least three observations'
    samples = [row['scd40'] for row in rows]
    assert all(s and s['ready'] and s['valid'] for s in samples), 'SCD40 absent or stale'
    assert all(row['uart_ready'] and row['sensor_valid'] for row in rows), 'Original sensors unhealthy'
    assert all(row['watchdog_state'] == 'healthy' for row in rows), 'Watchdog unhealthy'
    assert all(b['uptime_ms'] > a['uptime_ms'] for a, b in zip(rows, rows[1:])), 'Device reset'
    assert samples[-1]['samples'] >= samples[0]['samples'] + 2, 'Sampling stalled'
    assert all(s['serial'] and s['serial'] == samples[0]['serial'] for s in samples), 'Serial changed'
    assert all(s['co2_ppm'] > 0 and s['last_error'] == 0 for s in samples), 'Invalid CO2 / error'
    assert all(s['errors'] == samples[0]['errors'] and s['crc_errors'] == samples[0]['crc_errors'] for s in samples), 'I2C/CRC errors increased'
    assert all(row['uart_errors'] == rows[0]['uart_errors'] for row in rows), 'UART errors increased'
    print('PASS: continuous SCD40 samples, CRC, freshness, UART and watchdog')


if __name__ == '__main__':
    main()
