#!/usr/bin/env python3
"""Freeze sources and run the bounded short HTTP/MQTT profile once.

Dry-run is the default. Execution performs no OTA or device control/config
writes; the child runner sends only status.get requests.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/status101'
PRIVATE = BASE / 'private'
MIXED = ROOT / 'tests/device/mqtt_http_mixed.py'
DEFAULT_TARGETS = (
    '10.117.152.111=b0f893303134',
    '10.117.152.102=D0BAE463E12C',
    '10.117.152.110=b0f8932864b6',
)
SOURCES = {
    'controller.py': Path(__file__).resolve(),
    'mqtt_http_mixed.py': MIXED,
    'readonly_stress.py': ROOT / 'tests/device/readonly_stress.py',
    'readonly_mqtt_candidate.py': BASE / 'readonly_mqtt_candidate.py',
    'primary_stability_load.py': Path(
        '/home/fjr/hobbies/phicomm-community-firmware/tests/device/stability_load.py'),
}
PLAN = [
    {'name': 'mixed-1', 'http_clients': 1, 'mqtt_window': 1, 'seconds': 60},
    {'name': 'mixed-4', 'http_clients': 4, 'mqtt_window': 2, 'seconds': 120},
    {'name': 'mixed-8', 'http_clients': 8, 'mqtt_window': 4, 'seconds': 60},
    {'name': 'mixed-16', 'http_clients': 16, 'mqtt_window': 4, 'seconds': 60},
]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def private_json(path, value):
    temporary = path.with_suffix(path.suffix+'.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True)+'\n')
    temporary.chmod(0o600)
    temporary.replace(path)
    path.chmod(0o600)


def validate_label(label):
    if not isinstance(label, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,47}', label):
        raise ValueError('Invalid run label')
    return label


def validate_version(version):
    if not isinstance(version, str) or not re.fullmatch(r'0\.9\.15-[A-Za-z0-9][A-Za-z0-9.-]{0,63}', version):
        raise ValueError('Invalid candidate version')
    return version


def paths_for(label):
    output = PRIVATE / (label+'-mqtt-http-mixed')
    controller = PRIVATE / (label+'-mqtt-http-mixed-controller')
    return output, controller


def archive_sources(controller_dir):
    source_dir = controller_dir / 'sources'
    source_dir.mkdir(mode=0o700)
    records = {}
    for name, source in SOURCES.items():
        if not source.is_file():
            raise FileNotFoundError('Required runtime source is missing: '+name)
        destination = source_dir / name
        shutil.copyfile(source, destination)
        destination.chmod(0o600)
        records[name] = {
            'source_path': str(source),
            'archive_path': str(destination.relative_to(controller_dir)),
            'sha256': sha256(source),
            'archived_sha256': sha256(destination),
        }
    return records


def audit_sources(controller_dir, records):
    current, archived = {}, {}
    source_unchanged = True
    copies_unchanged = True
    for name, item in records.items():
        try:
            current_hash = sha256(item['source_path'])
        except OSError:
            current_hash = None
        archive_file = controller_dir / item['archive_path']
        try:
            archive_hash = sha256(archive_file)
        except OSError:
            archive_hash = None
        current[name], archived[name] = current_hash, archive_hash
        source_unchanged &= current_hash == item['sha256']
        copies_unchanged &= archive_hash == item['archived_sha256']
    return {
        'source_unchanged': bool(source_unchanged),
        'source_copies_unchanged': bool(copies_unchanged),
        'source_sha256_at_end': current,
        'archived_sha256_at_end': archived,
    }


def _target_map(specs=None):
    from tests.device.mqtt_http_mixed import validate_targets
    return validate_targets(list(DEFAULT_TARGETS if specs is None else specs))


def validate_child_result(child_exit, evidence, version, targets=None):
    from tests.device.mqtt_http_mixed import TARGETS as KNOWN_TARGETS
    selected = _target_map(targets)
    expected = {ip: {'device_id': ident,
                     'model': KNOWN_TARGETS[ip][1]}
                for ip, ident in selected.items()}
    all_ips = set(KNOWN_TARGETS)
    if child_exit != 0:
        raise ValueError('Mixed runner child did not exit successfully')
    if (not isinstance(evidence, dict) or evidence.get('status') != 'complete' or
            evidence.get('completed') is not True or evidence.get('version') != version or
            evidence.get('blockers') != [] or
            evidence.get('selected_targets') != list(selected) or
            evidence.get('targets') != expected or
            evidence.get('device_status') != {
                ip: ('SELECTED' if ip in selected else 'PAUSED') for ip in all_ips}):
        raise ValueError('Mixed runner evidence is missing, incomplete, or blocked')
    if evidence.get('http_prechecks') != 'passed' or evidence.get('http_postchecks') != 'passed':
        raise ValueError('Mixed runner HTTP gates are incomplete')
    phases = evidence.get('phases')
    if not isinstance(phases, list) or len(phases) != len(PLAN):
        raise ValueError('Mixed runner phase evidence is incomplete')
    for expected, actual in zip(PLAN, phases):
        if (actual.get('name') != expected['name'] or actual.get('completed') is not True or
                actual.get('seconds') != expected['seconds'] or
                actual.get('http_clients') != expected['http_clients'] or
                actual.get('mqtt_window') != expected['mqtt_window']):
            raise ValueError('Mixed runner phase plan/evidence mismatch')
    return True


def _child_failure_evidence(path):
    try:
        evidence = json.loads(path.read_text())
    except (OSError, ValueError):
        return False
    if not isinstance(evidence, dict):
        return False
    if evidence.get('status') == 'failed':
        return True
    return any(isinstance(row, dict) and row.get('hard_deadline_exceeded') is True
               for row in evidence.get('phases', []))


def wait_child(process, result_file, timeout, poll_interval=0.1, terminate_grace=1.0):
    """Bound child lifetime even if a non-daemon worker prevents Python exit."""
    deadline = time.monotonic()+timeout
    stopped_for_evidence = False
    while process.poll() is None:
        if _child_failure_evidence(result_file):
            stopped_for_evidence = True
            break
        remaining = deadline-time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(poll_interval, remaining))
    timed_out = process.poll() is None and not stopped_for_evidence
    if process.poll() is None:
        process.terminate()
        try:
            exit_code = process.wait(timeout=terminate_grace)
        except subprocess.TimeoutExpired:
            process.kill()
            exit_code = process.wait(timeout=terminate_grace)
    else:
        exit_code = process.returncode
    return exit_code, timed_out, stopped_for_evidence


def build_command(python, label, version, targets=None):
    command = [str(python), str(MIXED)]
    for target in (DEFAULT_TARGETS if targets is None else targets):
        command.extend(('--target', target))
    command.extend(('--version', version, '--label', label,
                    '--profile', 'short', '--execute'))
    return command


def run(label, version, execute=False, targets=None):
    label, version = validate_label(label), validate_version(version)
    selected = _target_map(targets)
    target_specs = [ip+'='+ident for ip, ident in selected.items()]
    output, controller_dir = paths_for(label)
    result_file = output / 'mixed-evidence.json'
    if output.exists() or controller_dir.exists():
        raise FileExistsError('Refusing to overwrite existing mixed-run outputs')
    if not execute:
        return {
            'mode': 'dry-run', 'label': label, 'version': version,
            'targets': target_specs, 'selected_targets': list(selected),
            'device_status': {ip: ('SELECTED' if ip in selected else 'PAUSED')
                              for ip in _target_map()},
            'profile': 'short', 'plan': PLAN,
            'output': str(output), 'controller_output': str(controller_dir),
            'will_connect': False, 'will_read_credentials': False,
            'will_send_ota': False, 'will_write_controls_or_config': False,
        }
    try:
        if importlib.util.find_spec('paho.mqtt.client') is None:
            raise RuntimeError('Use the prepared Python environment with paho-mqtt installed')
    except ModuleNotFoundError as exc:
        raise RuntimeError('Use the prepared Python environment with paho-mqtt installed') from exc

    controller_dir.mkdir(mode=0o700, parents=True)
    data = {
        'schema': 1, 'status': 'running', 'completed': False,
        'label': label, 'version': version, 'profile': 'short',
        'started_at': time.time(), 'targets': target_specs,
        'selected_targets': list(selected),
        'device_status': {ip: ('SELECTED' if ip in selected else 'PAUSED')
                          for ip in _target_map()}, 'plan': PLAN,
        'device_firmware_mutation': 'none', 'source_manifest': None,
        'process': None, 'blockers': [],
    }
    private_json(controller_dir/'controller.json', data)
    try:
        records = archive_sources(controller_dir)
        source_manifest = {
            'git_head': subprocess.run(
                ['git', 'rev-parse', 'HEAD'], cwd=ROOT, check=True,
                capture_output=True, text=True, timeout=5).stdout.strip(),
            'sources': records,
        }
        private_json(controller_dir/'source-manifest.json', source_manifest)
        command = build_command(sys.executable, label, version, target_specs)
        log_path = controller_dir/'child.log'
        data.update(source_manifest=source_manifest, command=command,
                    output=str(output), process_log=str(log_path))
        private_json(controller_dir/'controller.json', data)
        start = time.time()
        max_seconds = sum(row['seconds'] for row in PLAN) + len(PLAN)*90 + 120
        with log_path.open('wb') as log:
            os.chmod(log_path, 0o600)
            process = subprocess.Popen(
                command, cwd=ROOT, stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
                env=os.environ.copy())
            data['process'] = {'pid': process.pid, 'argv': command,
                               'started_at': start, 'timeout_seconds': max_seconds}
            private_json(controller_dir/'controller.json', data)
            child_exit, timed_out, stopped_for_evidence = wait_child(
                process, result_file, max_seconds)
        data['process'].update(ended_at=time.time(), exit_code=child_exit,
                               timed_out=timed_out,
                               terminated_after_failure_evidence=stopped_for_evidence)
        audit = audit_sources(controller_dir, records)
        data.update(audit)
        child_evidence = json.loads(result_file.read_text()) if result_file.is_file() else None
        data['child_status'] = child_evidence.get('status') if isinstance(child_evidence, dict) else None
        data['child_completed'] = (child_evidence.get('completed')
                                   if isinstance(child_evidence, dict) else False)
        try:
            validate_child_result(child_exit, child_evidence, version, target_specs)
            if timed_out:
                raise ValueError('Mixed runner process exceeded its bounded deadline')
            if stopped_for_evidence:
                raise ValueError('Mixed runner process was terminated after failed/incomplete evidence')
            if not audit['source_unchanged'] or not audit['source_copies_unchanged']:
                raise ValueError('A runtime source or archived copy changed during the run')
        except ValueError as exc:
            data['blockers'].append(str(exc))
        data.update(ended_at=time.time(), completed=not data['blockers'],
                    status='complete' if not data['blockers'] else 'failed')
        private_json(controller_dir/'controller.json', data)
        return data
    except BaseException as exc:
        data['blockers'].append(type(exc).__name__+': '+str(exc))
        records = (data.get('source_manifest') or {}).get('sources', {})
        if records:
            data.update(audit_sources(controller_dir, records))
        data.update(ended_at=time.time(), completed=False, status='failed')
        private_json(controller_dir/'controller.json', data)
        raise


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--target', action='append', metavar='IP=ID')
    parser.add_argument('--execute', action='store_true')
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        result = run(args.label, args.version, execute=args.execute, targets=args.target)
    except BaseException as exc:
        print(json.dumps({'status': 'failed', 'error_type': type(exc).__name__}, sort_keys=True))
        return 2
    print(json.dumps({key: result.get(key) for key in
                      ('mode', 'status', 'completed', 'label', 'version', 'targets',
                       'output', 'process', 'source_unchanged', 'source_copies_unchanged')
                      if key in result}, sort_keys=True))
    return 0 if result.get('completed') or result.get('mode') == 'dry-run' else 2


if __name__ == '__main__':
    raise SystemExit(main())
