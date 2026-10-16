"""Bounded, read-only M1 HTTP reset capture. Stops at the first request error."""
import concurrent.futures
import hashlib
import json
import os
import pathlib
import threading
import time
import urllib.error

from client import Device, redact

IP = '10.117.152.110'
ROOT = pathlib.Path(os.environ.get('M1_QA_ROOT', 'build/evidence/m1-qa/r7-rst-diag'))
UPLOAD = pathlib.Path(os.environ.get('M1_QA_UPLOAD', 'build/evidence/m1-r7-deploy/upload.json'))
SIGNED = pathlib.Path(os.environ.get('M1_QA_SIGNED', 'build/candidate-r7/m1/phicomm_m1.ota.bin.signed'))
ROOT.mkdir(parents=True, exist_ok=True, mode=0o700)
os.chmod(ROOT, 0o700)
LOCK = threading.Lock()
STOP = threading.Event()
FIRST_ERROR = []


def strict_json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate JSON key: ' + key)
            result[key] = value
        return result

    def reject_constant(value):
        raise ValueError('invalid JSON constant: ' + value)

    return json.loads(raw.decode('utf-8'), object_pairs_hook=pairs, parse_constant=reject_constant)


def parse_reply(raw):
    reply = strict_json(raw)
    if not isinstance(reply, dict) or not isinstance(reply.get('code'), str):
        raise ValueError('invalid command response envelope')
    if reply['code'] == 'ok' and not isinstance(reply.get('data'), dict):
        raise ValueError('successful command response has no object data')
    return reply


def save_json(name, value):
    path = ROOT / name
    path.write_text(json.dumps(redact(value), indent=2, ensure_ascii=False))
    path.chmod(0o600)


def log(name, value):
    path = ROOT / name
    with LOCK:
        with path.open('a') as stream:
            stream.write(json.dumps(redact({'utc': time.time(), **value}), ensure_ascii=False) + '\n')
        path.chmod(0o600)


def get_command(device, action):
    status, raw = device.request('/api/v1/command', {'action': action, 'params': {}})
    reply = parse_reply(raw)
    if status != 200 or reply['code'] != 'ok':
        raise RuntimeError('HTTP/API refusal: ' + str(status) + '/' + reply['code'])
    return reply['data']


def fail_once(stage, error):
    detail = {'stage': stage, 'error': str(error)}
    with LOCK:
        if not FIRST_ERROR:
            FIRST_ERROR.append(detail)
            STOP.set()


def main():
    upload = json.loads(UPLOAD.read_text())
    expected_version = (upload.get('manifest') or {}).get('version')
    signed_sha256 = hashlib.sha256(SIGNED.read_bytes()).hexdigest()
    if not isinstance(expected_version, str) or signed_sha256 != upload.get('sha256'):
        raise AssertionError('candidate manifest/hash mismatch')

    device = Device(IP, timeout=5)
    pre = get_command(device, 'status.get')
    if pre.get('device_id') != 'b0f8932864b6' or pre.get('version') != expected_version:
        raise AssertionError('M1 identity/version does not match candidate')
    if not (pre.get('wifi_connected') and pre.get('mqtt_connected') and pre.get('sensor_valid')
            and pre.get('age_ms', 15001) < 15000 and pre.get('mqtt_status') == 'ready'):
        raise AssertionError('M1 is not healthy enough for bounded diagnosis')
    save_json('pre-status.json', pre)
    save_json('candidate.json', {'signed_sha256': signed_sha256, 'expected_version': expected_version,
                                 'device_id': pre['device_id']})

    paths = ['/api/v1/status', '/api/v1/config', '/api/v1/schedules', '/api/v1/logs', '/',
             'binding.status.get']
    end = time.monotonic() + 30

    def worker(index):
        local = Device(IP, timeout=5)
        n = 0
        while not STOP.is_set() and time.monotonic() < end:
            path = paths[(index + n) % len(paths)]
            started = time.monotonic()
            try:
                if path.startswith('/api/'):
                    status, raw = local.request(path)
                    reply = parse_reply(raw)
                    ok = status == 200 and reply['code'] == 'ok'
                    if not ok:
                        raise RuntimeError('HTTP/API response: ' + str(status) + '/' + reply['code'])
                elif path == '/':
                    status, raw = local.request(path)
                    ok = status == 200 and bool(raw)
                    if not ok:
                        raise RuntimeError('HTTP response: ' + str(status))
                else:
                    data = get_command(local, path)
                    ok = isinstance(data, dict)
                log('requests.ndjson', {'client': index, 'path': path, 'ok': ok,
                                        'elapsed_seconds': time.monotonic() - started})
            except Exception as error:
                log('requests.ndjson', {'client': index, 'path': path, 'ok': False,
                                        'elapsed_seconds': time.monotonic() - started,
                                        'error': str(error)})
                fail_once('request', error)
                return
            n += 1
            STOP.wait(max(0, 1 - (time.monotonic() - started)))

    def health():
        local = Device(IP, timeout=5)
        while not STOP.is_set() and time.monotonic() < end:
            started = time.monotonic()
            try:
                status, raw = local.request('/api/v1/command', {'action': 'status.get', 'params': {}})
                reply = parse_reply(raw)
                if status != 200 or reply['code'] != 'ok':
                    raise RuntimeError('HTTP/API health: ' + str(status) + '/' + reply['code'])
                log('health.ndjson', {'ok': True, 'status': reply['data']})
            except Exception as error:
                log('health.ndjson', {'ok': False, 'error': str(error)})
                fail_once('health', error)
                return
            STOP.wait(max(0, 5 - (time.monotonic() - started)))

    started = time.monotonic()
    health_thread = threading.Thread(target=health, name='m1-rst-health')
    health_thread.start()
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(worker, range(8)))
    STOP.set()
    health_thread.join()
    summary = {'clients': 8, 'limit_seconds': 30, 'elapsed_seconds': time.monotonic() - started,
               'first_error': FIRST_ERROR[0] if FIRST_ERROR else None,
               'stopped_on_first_error': bool(FIRST_ERROR)}
    save_json('summary.json', summary)
    print(json.dumps(summary), flush=True)
    return 1 if FIRST_ERROR else 0


if __name__ == '__main__':
    raise SystemExit(main())
