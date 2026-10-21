"""Portable, single-incident accepted-upload proof. Never executes device I/O.

Original R2 controllers remain FAILED. This proof replaces neither their result nor
missing after.json: its after is explicitly an independent four-GET observation.
New runners must reverify this proof, collect three fresh guarded samples, then
skip only A1's upload. M1/TC1 still require normal signed OTA from 912413.
"""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = 'accepted-upload-independent-postcheck-v1'
CLEANUP_SHA256 = '7b20c5b5ac4a203e26ea282b6e23c0dfd0a1741b497cbbe531d3fa4a391c3cf4'
LABEL = 'shared4e5a85-diag-r2'
COMMIT = '4e5a85580188c72faa8790383286d8e4a3bd96df'
VERSION = '0.9.15-4e5a85'
PREVIOUS = '0.9.15-912413'
ARCHIVE = 'shared-4e5a85'
IP = '10.117.152.111'
IDENT = 'b0f893303134'
CAPTURE = 'phicomm-a1-ota-4c309d7fb660'
CONTROLS = ('socket_states', 'requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled')
# Pin the actual incident, rather than accepting an arbitrary newly shaped failure.
ANCHORS = {
 'shared4e5a85-diag-r2-initial-controller/controller.json': 'd6bac1c6795bf185adfe7b9a50f344c8dfb1e3ad968814f2e974f00102f50e7a',
 'shared4e5a85-diag-r2-diag-mqtt-failure/controller.json': '7acbfa8feb95bf4c99aea4c9a76e9284ec40bae302b7ffd74c1c432f0bcc6025',
 'shared4e5a85-diag-r2-upload-postcheck/evidence.json': '8b4da32a43a7563034c37a7770402f6b2bb4e4c29f1e75ddc9b354b25be42b1d',
 'deploy-shared4e5a85-diag-r2-initial-111/before.json': '1e907fe3da619cfb93a05e9364a6f54b2b13749f0ae02976137fd4e950adc195',
 'deploy-shared4e5a85-diag-r2-initial-111/upload-intent.json': '7fefda6862577a2bf75535f262257232925c4df4d7db78353897584e1bb4e91b',
 'deploy-shared4e5a85-diag-r2-initial-111/upload.json': 'af94fbf3e431f3e7176fc5c3d383cd6aa7d4350be326619721c7e233bac437c5',
}


def check(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load_json(data):
    return json.loads(data)


def _state(status, config, before):
    check(status.get('device_id', '').lower() == IDENT and status.get('model') == 'a1' and
          status.get('version') == VERSION, 'identity/version mismatch')
    check(type(status.get('uptime_seconds')) is int and status['uptime_seconds'] >= 0, 'invalid runtime uptime')
    check(config == before['config'], 'config changed')
    for key in CONTROLS:
        check((key == 'socket_states' or (key in status and key in before['status'])) and
              status.get(key) == before['status'].get(key),
              'control changed: '+key)
    check(status.get('firmware_variant') == 'full' and status.get('full_resources') is True and
          status.get('bootstrap_ui') is False, 'runtime is not full firmware')
    check(status.get('wifi_connected') is True and status.get('mqtt_connected') is True and
          status.get('ota_active') is False and status.get('reboot_pending') is False and
          status.get('ota_state') == 'idle' and status.get('ota_error') == '', 'runtime health missing/failed')


def _health(capabilities, memory):
    check(capabilities.get('model') == 'a1' and capabilities.get('firmware_variant') == 'full' and
          capabilities.get('full_resources') is True and capabilities.get('bootstrap_ui') is False and
          capabilities.get('status_views') == ['full', 'live'], 'capabilities missing/changed')
    for key, expected in (('http_core_owner_state', 1), ('http_core_restarts', 0),
                          ('http_core_close_failures', 0), ('mqtt_errors', 0), ('mqtt_reconnects', 0)):
        check(type(memory.get(key)) is int and memory[key] == expected, 'memory health failed: '+key)
    check(type(memory.get('free_memory')) is int and memory['free_memory'] > 0, 'missing free memory')


def verify_accepted_upload_postcheck(cleanup_file, *, root=None, validation_commit=None, check_current=True):
    """Verify immutable R2 + independent postcheck + independent local cleanup.

    cleanup_file must be a separately observed record, never synthesized from the
    failed capture.json. See verify_cleanup below. No current helper hash is
    required to match the old helper: its archived source is anchored instead.
    Current tracked worktree must be clean and HEAD may differ only in tests/docs,
    matching the existing initial-runner validation_tree policy.
    """
    root = Path(root or ROOT).resolve(strict=True)
    base = root/'build/status101'
    private = base/'private'
    check(Path(cleanup_file).resolve(strict=True) ==
          (private/(LABEL+'-upload-postcheck/cleanup-proof.json')).resolve(strict=True),
          'cleanup evidence is not the fixed incident path')
    proofs = {}

    def read(path, expected=None, json_data=True):
        path = Path(path).resolve(strict=True)
        check(path.is_relative_to(root), 'evidence outside root')
        data = path.read_bytes()
        sha = digest(data)
        check(expected is None or sha == expected, 'hash changed: '+str(path))
        proofs[str(path)] = sha
        return load_json(data) if json_data else data

    records = {name: read(private/name, sha) for name, sha in ANCHORS.items()}
    inner = records[LABEL+'-initial-controller/controller.json']
    outer = records[LABEL+'-diag-mqtt-failure/controller.json']
    post = records[LABEL+'-upload-postcheck/evidence.json']
    deploy = private/('deploy-'+LABEL+'-initial-111')
    before = records['deploy-'+LABEL+'-initial-111/before.json']
    intent = records['deploy-'+LABEL+'-initial-111/upload-intent.json']
    upload = records['deploy-'+LABEL+'-initial-111/upload.json']
    check(inner['status'] == outer['status'] == 'failed' and inner['failure_type'] == 'CaptureCleanupError' and
          inner['failure'] == 'RuntimeError: Could not determine owned capture container state' and
          inner['completed'] is False and inner['initial_signed_ota_complete'] is False and
          inner['paced_diagnostic_complete'] is False, 'original failure differs')
    for key in ('source_unchanged', 'source_copies_unchanged', 'source_manifest_unchanged', 'tracked_worktree_clean'):
        check(inner.get(key) is True, 'original source gate failed: '+key)
    check(outer['candidate_unchanged'] is True and outer['head_unchanged'] is True and
          outer['source_unchanged'] is True and outer['candidate_hashes'] == outer['candidate_hashes_at_end'] ==
          outer['frozen']['candidate'] and outer['source_hashes_at_start'] == outer['source_hashes_at_end'],
          'outer source/candidate changed')
    check(inner['firmware_commit'] == outer['candidate_commit'] == outer['frozen']['head'] == COMMIT and
          inner['version'] == outer['version'] == VERSION and inner['archive'] == outer['archive'] == ARCHIVE,
          'candidate identity differs')
    check(inner['previous_version'] == outer['previous_version'] == PREVIOUS, 'previous version differs')
    check(len(inner['steps']) == len(outer['stages']) == 1, 'not a single upload attempt')
    step, stage = inner['steps'][0], outer['stages'][0]
    check(step['argv'][2:] == ['111', LABEL+'-initial', PREVIOUS, ARCHIVE] and
          step['exit_code'] == 1 and stage['pid'] == inner['pid'] and stage['exit_code'] == 1 and
          stage['cleanup'] == {'groups_empty': True, 'remaining': {}, 'signals': []} and
          stage['timed_out'] is False and not stage['interrupted'] and not stage['disk_blocked'],
          'original process cleanup failed')
    check(set(private.glob('deploy-'+LABEL+'-initial-*/upload-intent.json')) == {deploy/'upload-intent.json'} and
          not (deploy/'after.json').exists(), 'unexpected upload/after record')
    check(step['started_at'] <= intent['at'] <= step['ended_at'] < post['started_at'] <= post['ended_at'],
          'upload/postcheck time order differs')
    check(upload.get('http') == 202 and load_json(upload['body']) == {'message': 'OTA verified; rebooting'} and
          upload.get('failed') is False and upload['request']['request_failed'] is False and
          upload['request']['request_exception'] is None and
          step['started_monotonic'] <= upload['request']['started_monotonic'] <=
          upload['request']['ended_monotonic'] <= upload['request']['response_read_monotonic'] <= step['ended_monotonic'],
          'upload was not normally accepted')
    check(before['status']['device_id'].lower() == IDENT and before['status']['model'] == 'a1' and
          before['status']['version'] == PREVIOUS, 'before identity differs')
    folder = private/(LABEL+'-initial-controller')
    manifest = read(folder/'source-manifest.json', inner['source_manifest_sha256'])
    check(manifest['candidate_commit'] == COMMIT and manifest['candidate_version'] == VERSION,
          'archived source identity differs')
    check(set(manifest['sources']) == set(inner['archived_hashes_at_end']) == set(inner['source_hashes_at_end']),
          'source archive incomplete')
    for name, item in manifest['sources'].items():
        path = (folder/item['archive_path']).resolve(strict=True)
        check(path.is_relative_to(folder.resolve()), 'source archive escape')
        check(item['sha256'] == item['archived_sha256'] == inner['archived_hashes_at_end'][name] ==
              inner['source_hashes_at_end'][name], 'source hash records differ')
        read(path, item['sha256'], False)
    # Verify all three normal P256 packages, not just the already uploaded A1.
    from tools.ota.package import verify_package
    verifier = root/'tools/ota/package.py'
    read(verifier, manifest['sources']['ota_package.py']['sha256'], False)
    read(root/'common/ota/keyring.h', json_data=False)
    a1_manifest = None
    for model, paths in outer['frozen']['candidate_paths'].items():
        check(model in ('a1', 'm1', 'tc1'), 'unknown candidate model')
        hashes = outer['candidate_hashes'][model]
        m = read(paths['manifest'], hashes['manifest_sha256'])
        raw = read(paths['raw'], hashes['raw_sha256'], False)
        signed = read(paths['signed'], hashes['signed_sha256'], False)
        meta = verify_package(signed, model, keyring_path=root/'common/ota/keyring.h')
        check(m['git_commit'] == meta['commit'] == COMMIT and m['version'] == VERSION and
              m['base_version'] == meta['version'] and m['dirty'] is False and
              m['firmware_variant'] == 'full' and signed[192:] == raw and
              m['files'][Path(paths['raw']).name] == digest(raw), 'P256/manifest mismatch')
        if model == 'a1':
            a1_manifest = m
            check(m['target'] == 'A1@MK3080B@moc' and intent['sha256'] == digest(signed) and
                  upload['request']['body_bytes'] == len(signed), 'A1 package differs')
    check(set(outer['frozen']['candidate_paths']) == {'a1', 'm1', 'tc1'} and
          intent['manifest'] == a1_manifest and intent['ip'] == IP and intent['device_id'].lower() == IDENT,
          'intent differs')
    check(post['status'] == 'observed' and post['overall_health_gate'] == 'PASS' and
          post['r2_overall_status'] == 'FAILED' and post['request_count'] == post['max_requests'] == 4 and
          post['baseline_sha256'] == proofs[str((deploy/'before.json').resolve())], 'postcheck gate differs')
    expected_endpoints = ['/api/v1/status', '/api/v1/config', '/api/v1/capabilities', '/api/v1/memory']
    check([e['endpoint'] for e in post['events']] == expected_endpoints, 'postcheck requests differ')
    states = []
    last_end = post['started_monotonic']
    for event in post['events']:
        check(event['http'] == 200 and event['outcome'] == 'success' and
              last_end <= event['request_start_mono'] <= event['request_end_mono'] <=
              post['started_monotonic']+post['elapsed_seconds']+0.01, 'postcheck request failed/timing invalid')
        raw = read(event['raw_file'], event['raw_sha256'], False)
        check(len(raw) == event['raw_bytes'], 'raw body size differs')
        envelope = load_json(raw)
        check(envelope['code'] == 'ok' and isinstance(envelope['data'], dict), 'invalid API body')
        states.append(envelope['data'])
        last_end = event['request_end_mono']
    status, config, caps, memory = states
    _state(status, config, before)
    _health(caps, memory)
    check(type(status.get('uptime_seconds')) is int and 0 < status['uptime_seconds'] < before['status']['uptime_seconds'] and
          intent['at']-2 <= post['ended_at']-status['uptime_seconds'] <= post['ended_at'], 'reboot boundary differs')
    capture = read(deploy/'a1-ota-trace/capture.json')
    check(capture['container_name'] == CAPTURE and capture['upload_attempt_finished'] is True and
          capture['status'] == 'failed' and capture['owned_container_cleanup'] is None,
          'capture failure shape differs')
    cleanup = read(cleanup_file, CLEANUP_SHA256)
    verify_cleanup(cleanup, post['ended_at'])
    check(cleanup['outer_controller_cleanup']['initial_stage_record'] == stage,
          'cleanup stage differs')
    for group, file_key, sha_key in (
        ('capture_container', 'intent_file', 'intent_sha256'),
        ('capture_container', 'original_capture_result_file', 'original_capture_result_sha256'),
        ('original_initial_controller', 'file', 'sha256'),
        ('outer_controller_cleanup', 'file', 'sha256'),
        ('postcheck', 'evidence_file', 'evidence_sha256'),
        ('postcheck', 'helper_file', 'helper_sha256'),
        ('postcheck', 'readonly_request_helper_file', 'readonly_request_helper_sha256')):
        item = cleanup[group]
        read(root/item[file_key], item[sha_key], False)
    head = validation_commit or subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True, timeout=5).strip()
    dirty = ''
    if check_current:
        current = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True, timeout=5).strip()
        check(head == current, 'validation HEAD is not current')
        dirty = subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=root, text=True, timeout=5)
    check(len(head) == 40 and all(c in '0123456789abcdef' for c in head), 'invalid validation commit')
    changed = subprocess.check_output(['git', 'diff', '--name-only', COMMIT, head], cwd=root, text=True, timeout=5).splitlines()
    check(not dirty and
          all(p.startswith(('tests/', 'docs/')) for p in changed), 'current firmware source changed')
    # Anchor logs and capture artifacts too; never rewrite historical records.
    for path in (base/(step['name']+'.log'), Path(stage['log']), deploy/'memory-before-upload.json',
                 deploy/'a1-ota-trace/capture-intent.json', deploy/'a1-ota-trace/a1-ota.pcap',
                 deploy/'a1-ota-trace/a1-ota.tcp.txt'):
        read(path, json_data=False)
    return {'schema': SCHEMA, 'source_label': LABEL,
            'ip': IP, 'archive': ARCHIVE, 'commit': COMMIT, 'version': VERSION, 'previous_version': PREVIOUS,
            'validation_commit': head, 'candidate_validation_paths': changed,
            'original_inner_status': 'failed', 'original_outer_status': 'failed', 'original_failed_step': step,
            'original_step': {'name': 'independent-upload-postcheck', 'started_at': post['started_at'], 'ended_at': post['ended_at']},
            'postcheck_step': {'started_at': post['started_at'], 'ended_at': post['ended_at']},
            'after_basis': 'independent four-GET postcheck; historical after.json absent',
            'after': {'status': status, 'config': config}, 'capabilities': caps, 'memory': memory,
            'cleanup_file': str(Path(cleanup_file).resolve()), 'proof_sha256': dict(sorted(proofs.items()))}


def verify_cleanup(record, not_before):
    """Validate the actual separately observed cleanup record, not failed capture."""
    check(record.get('schema') == 'shared4e5a85-diag-r2-cleanup-proof-v1' and
          record.get('created_at_epoch', 0) >= not_before and
          record.get('all_owned_process_groups_empty') is True and record.get('all_pids_absent') is True,
          'independent cleanup missing')
    container = record['capture_container']
    inspect = container['inspect']
    stderr = 'Error response from daemon: No such container: '+CAPTURE+'\n'
    check(container['name'] == CAPTURE and container['original_capture_status'] == 'failed' and
          inspect['argv'] == ['docker', 'inspect', '--type', 'container', CAPTURE] and
          inspect['returncode'] == 1 and inspect['outcome'] == 'absent' and
          inspect['stderr'] == stderr and inspect['stderr_sha256'] == digest(stderr.encode()) and
          inspect['stdout_bytes'] == 3 and inspect['stdout_sha256'] == digest(b'[]\n') and
          0 <= inspect['elapsed_seconds'] <= 10, 'container absence not proven')
    check(record['old_owned_pids'] == {str(pid): {'proc_present': False} for pid in
                                      (129748, 129751, 129757, 129762)}, 'old PID remains/missing')
    outer = record['outer_controller_cleanup']
    check(outer['groups_empty'] is True and outer['remaining'] == {} and
          outer['initial_stage_record']['cleanup'] == {'groups_empty': True, 'remaining': {}, 'signals': []},
          'process group cleanup not proven')


