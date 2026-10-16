#!/usr/bin/env python3
"""One-shot, explicitly authorized unsigned rescue OTA for the two test DUTs."""
import argparse
import ast
import hashlib
import http.client
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests' / 'device'))
from client import Device, redact, save

DEVICES = {
    'a1': {'ip': '10.117.152.111', 'id': 'b0f893303134', 'ready': 'panel_ready'},
    'm1': {'ip': '10.117.152.110', 'id': 'b0f8932864b6', 'ready': 'sensor_valid'},
}
EXPECTED_BASE = '0.9.5-3eddfd-dirty'
PRESERVED_CONFIG = ('name', 'ssid', 'mqtt_host', 'mqtt_port', 'mqtt_user',
                    'wifi_password_set', 'mqtt_password_set', 'web_password_set')


def save_private(path, value):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.parent.chmod(0o700)
    save(path, redact(value), private=True)


def validate_candidate(model, candidate):
    candidate = pathlib.Path(candidate).resolve()
    manifest = json.loads((candidate / 'manifest.json').read_text())
    name = f'phicomm_{model}.ota.bin'
    image = candidate / name
    raw = image.read_bytes()
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    expected_version = f"{manifest.get('base_version')}-{commit[:6]}-dirty"
    if manifest.get('target') != f'{model.upper()}@MK3080B@moc':
        raise ValueError('Candidate target does not match selected device')
    if manifest.get('git_commit') != commit or manifest.get('version') != expected_version or manifest.get('dirty') is not True:
        raise ValueError('Candidate manifest does not match this dirty source checkout')
    digest = hashlib.sha256(raw).hexdigest()
    if (manifest.get('files', {}).get(name) != digest or
            manifest.get('image', {}).get('sha256') != digest or
            manifest.get('image', {}).get('size') != len(raw)):
        raise ValueError('Raw image does not match its manifest')
    checker = ROOT / model.upper() / 'tools' / 'check_image.py'
    image_check = ast.literal_eval(subprocess.check_output(
        [sys.executable, str(checker), str(image)], cwd=ROOT, text=True))
    if image_check.get('sha256') != digest:
        raise ValueError('Native image validation returned a different digest')
    return manifest, image, digest, image_check


def healthy(status, model, before_uptime=None):
    identity = DEVICES[model]
    return (status.get('device_id') == identity['id'] and
            status.get('version') == EXPECTED_BASE and
            isinstance(status.get('last_reset_reason'), int) and
            status.get('wifi_connected') is True and
            status.get('mqtt_connected') is True and
            status.get(identity['ready']) is True and
            status.get('ota_active') is False and
            (before_uptime is None or status.get('uptime_ms', before_uptime) < before_uptime))


def verify_previous_deployment(model, evidence, digest):
    if evidence is None:
        raise ValueError('An R18 DUT requires the explicit previous deployment evidence path')
    evidence = pathlib.Path(evidence).resolve()
    candidate = json.loads((evidence / 'candidate.json').read_text())
    upload = json.loads((evidence / 'upload.json').read_text())
    after = json.loads((evidence / 'after.json').read_text())
    if (candidate.get('endpoint') != '/api/v1/ota/upload-unsigned' or
            candidate.get('signature_skipped_by_user_authorization') is not True or
            candidate.get('raw_sha256') != upload.get('raw_sha256') or
            upload.get('http_status') != 202 or candidate.get('raw_sha256') == digest):
        raise ValueError('Previous evidence does not prove a distinct successful unsigned deployment')
    status = after.get('status', {})
    if (status.get('device_id') != DEVICES[model]['id'] or
            status.get('version') != EXPECTED_BASE or
            not isinstance(status.get('last_reset_reason'), int)):
        raise ValueError('Previous after-deployment evidence does not identify this R18 DUT')


def deploy(model, candidate, evidence, previous_evidence=None):
    manifest, image, digest, image_check = validate_candidate(model, candidate)
    identity = DEVICES[model]
    device = Device(identity['ip'], timeout=8)
    before = device.get()
    config = device.get('config.get')
    if before.get('device_id') != identity['id']:
        raise ValueError('Connected DUT identity does not match selected model')
    if before.get('version') != EXPECTED_BASE:
        raise ValueError('DUT firmware version does not match the recorded baseline')
    if 'last_reset_reason' in before:
        verify_previous_deployment(model, previous_evidence, digest)
    elif previous_evidence is not None:
        raise ValueError('Previous R18 evidence was supplied but the DUT has no R18 boot marker')
    if (not before.get('wifi_connected') or not before.get('mqtt_connected') or
            not before.get(identity['ready']) or before.get('ota_active') or
            before.get('ota_state') != 'idle'):
        raise ValueError('DUT is not healthy and idle before OTA')
    if config.get('web_password_set'):
        raise ValueError('Web authentication is enabled; this helper has no credential path')

    save_private(evidence / 'candidate.json', {
        'manifest': manifest, 'raw_path': str(image), 'raw_sha256': digest,
        'raw_bytes': image.stat().st_size, 'native_image_check': image_check,
        'endpoint': '/api/v1/ota/upload-unsigned', 'signature_skipped_by_user_authorization': True,
    })
    save_private(evidence / 'before.json', {'status': before, 'config': config})

    connection = http.client.HTTPConnection(identity['ip'], timeout=60)
    try:
        connection.request('POST', '/api/v1/ota/upload-unsigned', image.read_bytes(),
                           {'Content-Type': 'application/octet-stream'})
        response = connection.getresponse()
        body = response.read().decode('utf-8', errors='replace')
        upload = {'http_status': response.status, 'body': body, 'raw_sha256': digest,
                  'raw_bytes': image.stat().st_size, 'manifest': manifest}
        save_private(evidence / 'upload.json', upload)
    except OSError as error:
        save_private(evidence / 'upload-transport-unknown.json', {
            'error': repr(error), 'raw_sha256': digest,
            'note': 'Do not retry until DUT state is inspected.',
        })
        raise
    finally:
        connection.close()
    print(model, 'unsigned rescue upload response', upload['http_status'], flush=True)
    if upload['http_status'] != 202:
        raise RuntimeError('Device rejected the unsigned OTA upload')

    after = device.wait(lambda status: healthy(status, model, before['uptime_ms']), seconds=180)
    after_config = device.get('config.get')
    for key in PRESERVED_CONFIG:
        if config.get(key) != after_config.get(key):
            raise AssertionError(f'Configuration changed across OTA: {key}')
    save_private(evidence / 'after.json', {'status': after, 'config': after_config})
    print(model, 'R18 boot marker, restart, peripherals and preserved config verified',
          'reset_reason=', after['last_reset_reason'], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', choices=DEVICES, required=True)
    parser.add_argument('--candidate', type=pathlib.Path, required=True)
    parser.add_argument('--evidence', type=pathlib.Path, required=True)
    parser.add_argument('--previous-evidence', type=pathlib.Path,
                        help='Required when replacing a previously verified R18 unsigned image')
    args = parser.parse_args()
    deploy(args.model, args.candidate, args.evidence, args.previous_evidence)


if __name__ == '__main__':
    main()
