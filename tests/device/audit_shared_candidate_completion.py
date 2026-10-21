#!/usr/bin/env python3
"""Fail-closed, offline completion audit for a shared HTTP candidate run.

This checks recorded evidence only. A passing report is not release approval and
does not establish leak freedom or replace independent device validation.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys


TARGETS = {
    '10.117.152.111': ('b0f893303134', 'a1'),
    '10.117.152.102': ('D0BAE463E12C', 'tc1'),
    '10.117.152.110': ('b0f8932864b6', 'm1'),
}
PHASES = {
    'concurrency-1': (1, 600),
    'concurrency-4': (4, 600),
    'concurrency-8': (8, 120),
    'concurrency-16': (16, 120),
    'soak': (4, 14400),
}
FULL_STEPS = [
    'concurrency-1', 'concurrency-4', 'concurrency-8', 'concurrency-16',
    'slow-clients', 'aborted-clients', 'buffered-read-clients',
    'missing-read-clients', 'soak',
]
SCENARIO_FOR_STEP = {
    'slow-clients': 'slow-clients',
    'aborted-clients': 'aborted-clients',
    'buffered-read-clients': 'buffered-read-clients',
    'missing-read-clients': 'missing-read-clients',
    'concurrency-1': 'concurrency-1',
    'concurrency-4': 'concurrency-4',
    'concurrency-8': 'concurrency-8',
    'concurrency-16': 'concurrency-16',
    'soak': 'soak',
}
READ_ACTIONS = {
    'status.get', 'capabilities.get', 'logs.get', 'config.get',
    'power.get', 'binding.status.get',
}
ZERO_RESOURCE_KEYS = (
    'json_live_blocks', 'memory_active', 'memory_reserved', 'memory_waiting',
)
OWNER_KEYS = (
    'http_core_owner_state', 'http_core_close_failures', 'http_core_restarts',
    'http_core_tcp_limit', 'http_core_tcp_rc', 'http_core_occupied',
    'http_core_server_bytes', 'http_tcp_sample_ms',
)
MODEL_CAP = {'a1': 3, 'tc1': 4, 'm1': 4}


def _is_int(value):
    return type(value) is int


def _is_number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _steady_memory_data(data, model):
    if not isinstance(data, dict):
        return False
    if not all(_is_int(data.get(key)) and data[key] == 0 for key in ZERO_RESOURCE_KEYS):
        return False
    required = ('http_core_owner_state', 'http_core_close_failures',
                'http_core_restarts', 'http_core_tcp_limit', 'http_core_tcp_rc',
                'http_core_occupied', 'http_core_server_bytes', 'http_tcp_sample_ms')
    if not all(_is_int(data.get(key)) for key in required):
        return False
    return (data['http_core_owner_state'] == 1 and
            data['http_core_close_failures'] == 0 and
            data['http_core_restarts'] == 0 and
            data['http_core_tcp_limit'] == MODEL_CAP[model] and
            data['http_core_tcp_rc'] == 0 and
            0 <= data['http_core_occupied'] <= 1 and
            data['http_core_server_bytes'] > 0 and
            data['http_tcp_sample_ms'] >= 0)


def _resource_active(data):
    return isinstance(data, dict) and any(_is_int(data.get(key)) and data[key] > 0
                                           for key in ZERO_RESOURCE_KEYS)


def _owner_telemetry_complete(data):
    return isinstance(data, dict) and all(_is_int(data.get(key)) for key in OWNER_KEYS)


def _memory_uptime_gate(rows, before_record):
    state = before_record.get('state') if isinstance(before_record, dict) else None
    state = state if isinstance(state, dict) else {}
    seed_start = before_record.get('status_request_started_monotonic') if isinstance(before_record, dict) else None
    seed_end = before_record.get('status_request_ended_monotonic') if isinstance(before_record, dict) else None
    seed_uptime = state.get('uptime_seconds')
    seed_valid = (_is_number(seed_start) and _is_number(seed_end) and
                  seed_end >= seed_start and _is_number(seed_uptime) and seed_uptime >= 0)
    samples = []
    valid = seed_valid
    for row in rows:
        if row.get('ok') is not True or row.get('http') != 200:
            continue
        data = row.get('data')
        start = row.get('request_started_monotonic')
        end = row.get('request_ended_monotonic')
        uptime = data.get('uptime_ms') if isinstance(data, dict) else None
        if (not _is_number(start) or not _is_number(end) or end < start or
                not _is_int(uptime) or uptime < 0):
            valid = False
            continue
        samples.append((start, end, uptime))
    samples.sort(key=lambda item: (item[0], item[1]))
    seed_bound_checks = 0
    nonoverlap_pairs = 0
    previous_end = previous_uptime = None
    for start, end, uptime in samples:
        if seed_valid and start >= seed_end:
            seed_bound_checks += 1
            if uptime < seed_uptime * 1000:
                valid = False
        if previous_end is not None and previous_end <= start:
            nonoverlap_pairs += 1
            if uptime < previous_uptime:
                valid = False
        if previous_end is None or end > previous_end:
            previous_end, previous_uptime = end, uptime
    return {'ok': valid and bool(rows) and bool(samples),
            'samples': len(samples), 'seed_bound_checks': seed_bound_checks,
            'nonoverlap_pairs': nonoverlap_pairs}


def _audit(evidence, plan, process_exit, source_bytes, label, version):
    reasons = []
    checks = {}

    def require(condition, code):
        if not condition:
            reasons.append(code)

    require(bool(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}', label)), 'invalid_label')
    require(bool(re.fullmatch(r'0\.9\.15-[A-Za-z0-9][A-Za-z0-9.-]{0,63}', version)), 'invalid_version')

    expected_targets = [
        {'ip': ip, 'device_id': ident, 'model': model, 'version': version}
        for ip, (ident, model) in TARGETS.items()
    ]
    plan_data = (evidence.get('run') or {}).get('plan') or {}
    planned_targets = plan_data.get('targets')
    normalized = []
    if isinstance(planned_targets, list):
        for item in planned_targets:
            if isinstance(item, dict):
                normalized.append({
                    'ip': item.get('ip'), 'device_id': item.get('device_id'),
                    'model': TARGETS.get(item.get('ip'), (None, None))[1],
                    'version': item.get('version'),
                })
    require(normalized == expected_targets, 'target_identity_model_or_version_mismatch')
    require(evidence.get('version') == version, 'evidence_version_mismatch')
    versions = evidence.get('target_versions')
    require(isinstance(versions, dict) and all(
        versions.get(ip, version) == version for ip in TARGETS), 'target_versions_mismatch')
    run = evidence.get('run') or {}
    require(run.get('status') == 'complete' and run.get('completed') is True,
            'run_not_completed')
    require(run.get('failure_type') is None, 'run_has_failure_type')
    require(run.get('plan_completion_required') is True, 'planned_completion_not_required')
    require(run.get('require_shared_core') is True, 'shared_core_gate_not_required')
    require(evidence.get('blockers') == [], 'run_has_blockers')
    checks['run'] = run.get('status') == 'complete' and run.get('completed') is True

    targets_in_runner_plan = plan_data.get('targets', [])
    plan_map = {item.get('ip'): item for item in targets_in_runner_plan if isinstance(item, dict)}
    require(set(plan_map) == set(TARGETS), 'runner_plan_target_set_mismatch')
    require(plan_data.get('mode') == 'full', 'not_full_stability_plan')
    require(plan_data.get('steps') == FULL_STEPS, 'planned_steps_mismatch')
    if plan_data.get('soak_period') != 1:
        reasons.append('soak_period_not_one_second')
    if plan_data.get('soak_seconds') != 14400 or plan_data.get('soak_clients') != 4:
        reasons.append('soak_plan_mismatch')
    planned_phases = plan_data.get('phases', [])
    phase_plan_ok = True
    for name, (clients, seconds) in PHASES.items():
        records = [p for p in planned_phases if isinstance(p, dict) and p.get('name') == name]
        phase_plan_ok &= len(records) == 1 and records[0].get('clients_per_device') == clients and records[0].get('requested_seconds') == seconds
    require(phase_plan_ok and len(planned_phases) == len(PHASES), 'phase_plan_mismatch')
    checks['plan'] = not any(code in reasons for code in (
        'runner_plan_target_set_mismatch', 'not_full_stability_plan', 'planned_steps_mismatch',
        'soak_period_not_one_second', 'soak_plan_mismatch', 'phase_plan_mismatch'))

    argv = (plan.get('process') or {}).get('argv', [])
    pid = (plan.get('process') or {}).get('pid')
    executable = Path(argv[0]).name if isinstance(argv, list) and argv else ''
    script_entry = isinstance(argv, list) and len(argv) > 1 and argv[1] == 'tests/device/readonly_stress.py'
    module_entry = isinstance(argv, list) and len(argv) > 2 and argv[1:3] == ['-m', 'tests.device.readonly_stress']
    require(isinstance(argv, list) and executable in ('python', 'python3') and
            (script_entry or module_entry), 'process_argv_missing_or_unexpected')
    for flag, expected in (
        ('--version', version), ('--normal-phase-seconds', '600'),
        ('--phase-seconds', '120'), ('--soak-seconds', '14400'),
        ('--soak-clients', '4'), ('--soak-period', '1'),
    ):
        matches = [argv[i + 1] for i, value in enumerate(argv[:-1]) if value == flag]
        require(matches == [expected], 'process_argument_mismatch:'+flag)
    cli_targets = [argv[i + 1] for i, value in enumerate(argv[:-1]) if value == '--target']
    require(cli_targets == [ip+'='+ident for ip, (ident, _) in TARGETS.items()], 'process_targets_mismatch')
    target_version_args = [argv[i + 1] for i, value in enumerate(argv[:-1])
                           if value == '--target-version']
    require(target_version_args in ([], [ip+'='+version for ip in TARGETS]),
            'process_target_version_overrides_mismatch')
    require(argv.count('--require-shared-core') == 1, 'shared_core_cli_gate_missing')
    expected_phase_plan = {name: seconds for name, (_, seconds) in PHASES.items()}
    require(plan.get('phase_plan_seconds') == expected_phase_plan, 'external_phase_plan_mismatch')
    require(isinstance(pid, int) and pid > 0, 'process_pid_missing')
    require(process_exit.get('pid') == pid and process_exit.get('exit_code') == 0,
            'process_exit_not_zero_or_pid_mismatch')
    require(process_exit.get('runner_source_unchanged') is True, 'runner_source_changed_during_run')
    digest = hashlib.sha256(source_bytes).hexdigest() if source_bytes is not None else None
    require(bool(digest) and digest == plan.get('runner_sha256'), 'archived_runner_hash_mismatch')
    checks['process_exit'] = process_exit.get('pid') == pid and process_exit.get('exit_code') == 0 and process_exit.get('runner_source_unchanged') is True

    before = evidence.get('before')
    identity_ok = isinstance(before, dict) and set(before) == set(TARGETS)
    if identity_ok:
        for ip, (ident, model) in TARGETS.items():
            record = before.get(ip) or {}
            state = record.get('state') or {}
            expected_version = version
            identity_ok &= str(record.get('id', '')).lower() == ident.lower()
            identity_ok &= str(state.get('device_id', '')).lower() == ident.lower()
            identity_ok &= state.get('model') == model and state.get('version') == expected_version
            identity_ok &= isinstance(record.get('config'), dict)
    require(identity_ok, 'initial_identity_version_model_or_config_missing')
    checks['initial_identity'] = bool(identity_ok)

    completion = evidence.get('completion_checks') or {}
    gate_names = ('network_restore', 'configuration_unchanged', 'idle_quiescence')
    require(all(completion.get(key) is True for key in gate_names), 'completion_gate_missing_or_false')
    require(run.get('ended_at', 0) >= run.get('started_at', float('inf')), 'invalid_run_time_bounds')
    checks['runner_gates'] = all(completion.get(key) is True for key in gate_names)

    phases = evidence.get('phases') if isinstance(evidence.get('phases'), list) else []
    phase_results = {}
    for name, (clients, seconds) in PHASES.items():
        matches = [p for p in phases if isinstance(p, dict) and p.get('name') == name]
        ok = len(matches) == 1
        if ok:
            item = matches[0]
            ok = (item.get('completed') is True and item.get('clients_per_device') == clients and
                  item.get('requested_seconds') == seconds and
                  isinstance(item.get('elapsed_seconds'), (int, float)) and item['elapsed_seconds'] >= seconds)
        require(ok, 'phase_incomplete_or_wrong_duration:'+name)
        phase_results[name] = bool(ok)
    checks['phases'] = all(phase_results.values())

    scenarios = evidence.get('scenarios') if isinstance(evidence.get('scenarios'), list) else []
    scenario_results = {}
    for step, scenario_name in SCENARIO_FOR_STEP.items():
        matches = [s for s in scenarios if isinstance(s, dict) and s.get('name') == scenario_name]
        ok = len(matches) == 1 and matches[0].get('completed') is True
        require(ok, 'scenario_incomplete_or_missing:'+scenario_name)
        scenario_results[scenario_name] = bool(ok)
    checks['scenarios'] = all(scenario_results.values())

    recoveries = evidence.get('recoveries') if isinstance(evidence.get('recoveries'), list) else []
    recovery_specs = [
        {'name': name+'-recovery', 'targets': list(TARGETS)} for name in PHASES
    ] + [
        {'name': name, 'targets': list(TARGETS)} for name in (
            'incomplete-headers-recovery', 'aborted-connections-recovery',
            'buffered-read-input-recovery', 'missing-read-eof-recovery')
    ]
    recovery_results = {}
    for spec in recovery_specs:
        matches = [r for r in recoveries if isinstance(r, dict) and r.get('name') == spec['name']]
        covered = set()
        ok = bool(matches)
        for record in matches:
            target_records = record.get('targets')
            if record.get('completed') is not True or not isinstance(target_records, dict):
                ok = False
                continue
            for ip, result in target_records.items():
                if ip not in spec['targets'] or ip in covered or not isinstance(result, dict):
                    ok = False
                    continue
                if (result.get('completed') is not True or result.get('consecutive_successes', 0) < 3):
                    ok = False
                covered.add(ip)
        ok &= covered == set(spec['targets'])
        require(ok, 'recovery_incomplete_or_missing:'+spec['name'])
        recovery_results[spec['name']] = bool(ok)
    checks['recoveries'] = all(recovery_results.values())

    rows = evidence.get('rows') if isinstance(evidence.get('rows'), list) else []
    http_counts = {'200': 0, '503': 0, 'other': 0, 'no_http': 0}
    illegal_http_rows = 0
    unexpected_200_rows = 0
    uptime_ok = {ip: True for ip in TARGETS}
    uptime_samples = {ip: [] for ip in TARGETS}
    for row in rows:
        if not isinstance(row, dict):
            reasons.append('malformed_request_row')
            continue
        ip = row.get('ip')
        if ip not in TARGETS:
            reasons.append('row_for_unplanned_target')
            continue
        action = row.get('action')
        if action not in READ_ACTIONS:
            reasons.append('non_readonly_action_in_evidence')
        status = row.get('http')
        if status == 200:
            http_counts['200'] += 1
            if row.get('code') not in ('ok', 'busy', 'invalid_params'):
                unexpected_200_rows += 1
            if action == 'status.get' and row.get('ok') is True:
                up = row.get('uptime')
                if isinstance(up, (int, float)):
                    start = row.get('at')
                    duration_ms = row.get('ms')
                    if (isinstance(start, (int, float)) and
                            isinstance(duration_ms, (int, float)) and duration_ms >= 0):
                        uptime_samples[ip].append((start, start+duration_ms/1000.0, up))
        elif status == 503:
            http_counts['503'] += 1
        elif status is None:
            http_counts['no_http'] += 1
        else:
            http_counts['other'] += 1
            illegal_http_rows += 1
    require(bool(rows), 'request_rows_missing')
    require(illegal_http_rows == 0, 'illegal_http_status_rows')
    require(unexpected_200_rows == 0, 'unexpected_http_200_result')
    for ip, samples in uptime_samples.items():
        samples.sort(key=lambda item: item[0])
        previous_end, previous_uptime = None, None
        start_uptime = ((before.get(ip) or {}).get('state') or {}).get('uptime_seconds') if isinstance(before, dict) else None
        run_start = run.get('started_at')
        anchor_ok = isinstance(start_uptime, (int, float)) and isinstance(run_start, (int, float))
        for start, end, uptime in samples:
            # The reported uptime should remain above the baseline-derived boot
            # lower bound, even when request concurrency obscures sample order.
            if anchor_ok:
                elapsed = max(0.0, start-run_start)
                if uptime < start_uptime + elapsed - 6.0:
                    uptime_ok[ip] = False
                    break
            # Uptime is comparable only when request intervals do not overlap.
            if (previous_end is not None and previous_end <= start and
                    previous_uptime is not None and uptime < previous_uptime):
                uptime_ok[ip] = False
                break
            if previous_end is None or end > previous_end:
                previous_end, previous_uptime = end, uptime
        if not anchor_ok:
            uptime_ok[ip] = False
        require(len(samples) > 0 and uptime_ok[ip], 'uptime_missing_or_decreased:'+ip)
    checks['http_and_uptime'] = illegal_http_rows == 0 and unexpected_200_rows == 0 and all(uptime_ok.values())

    memory = evidence.get('memory') if isinstance(evidence.get('memory'), list) else []
    memory_results = {}
    memory_counts = {}
    memory_uptime_results = {}
    for ip, (_, model) in TARGETS.items():
        device_rows = [r for r in memory if isinstance(r, dict) and r.get('ip') == ip]
        successful = [r for r in device_rows if r.get('ok') is True and r.get('http') == 200 and isinstance(r.get('data'), dict)]
        baseline = [r['data'] for r in successful if r.get('phase') == 'baseline']
        final = [r['data'] for r in successful if r.get('phase') == 'final']
        baseline_steady = [r['data'] for r in successful
                           if r.get('phase') == 'baseline' and _steady_memory_data(r['data'], model)]
        final_steady = [r['data'] for r in successful
                        if r.get('phase') == 'final' and _steady_memory_data(r['data'], model)]
        ok = len(baseline) >= 8 and len(final) >= 8
        if ok:
            for window, values in (('baseline', baseline), ('final', final)):
                for data in values[-3:]:
                    ok &= all(_is_int(data.get(key)) and data.get(key) == 0 for key in ZERO_RESOURCE_KEYS)
                    # The memory endpoint's own HTTP request can occupy one
                    # shared slot while it captures diagnostics.
                    ok &= _is_int(data.get('http_core_occupied')) and 0 <= data['http_core_occupied'] <= 1
                    if 'mqtt_pending' in data:
                        ok &= _is_int(data['mqtt_pending']) and data['mqtt_pending'] == 0
            cap = MODEL_CAP[model]
            for sample in successful:
                data = sample['data']
                ok &= all(_is_int(data.get(key)) for key in OWNER_KEYS)
                if all(_is_int(data.get(key)) for key in OWNER_KEYS):
                    ok &= data['http_core_owner_state'] == 1
                    ok &= data['http_core_close_failures'] == 0
                    ok &= data['http_core_restarts'] == 0
                    ok &= data['http_core_tcp_limit'] == cap and data['http_core_tcp_rc'] == 0
                    ok &= 0 <= data['http_core_occupied'] <= cap and data['http_core_server_bytes'] > 0
            ok &= len(baseline_steady) >= 3 and len(final_steady) >= 3
            base_mem = [d.get('free_memory') for d in baseline_steady
                        if _is_int(d.get('free_memory'))]
            final_mem = [d.get('free_memory') for d in final_steady
                         if _is_int(d.get('free_memory'))]
            ok &= len(base_mem) >= 3 and len(final_mem) >= 3
            loss = statistics.median(base_mem)-statistics.median(final_mem) if base_mem and final_mem else None
            threshold_pass = loss is not None and loss <= 1024
        else:
            loss, threshold_pass = None, False
        if len(baseline_steady) < 3 or len(final_steady) < 3:
            require(False, 'insufficient_steady_memory_samples:'+ip)
        uptime_result = _memory_uptime_gate(device_rows, before.get(ip, {})
                                            if isinstance(before, dict) else {})
        memory_uptime_results[ip] = bool(uptime_result['ok'])
        require(uptime_result['ok'], 'memory_uptime_missing_or_decreased:'+ip)
        require(ok, 'memory_or_shared_owner_recovery_invalid:'+ip)
        require(threshold_pass, 'coarse_heap_threshold_failed:'+ip)
        memory_results[ip] = bool(ok)
        memory_counts[ip] = {
            'baseline_samples': len(baseline), 'final_samples': len(final),
            'baseline_steady_samples': len(baseline_steady),
            'final_steady_samples': len(final_steady),
            'baseline_nonsteady_samples': len(baseline) - len(baseline_steady),
            'final_nonsteady_samples': len(final) - len(final_steady),
            'baseline_resource_active_samples': sum(1 for d in baseline if _resource_active(d)),
            'final_resource_active_samples': sum(1 for d in final if _resource_active(d)),
            'baseline_resource_zero_owner_unverified_samples': sum(
                1 for d in baseline if all(_is_int(d.get(k)) and d[k] == 0
                                          for k in ZERO_RESOURCE_KEYS) and
                not _owner_telemetry_complete(d)),
            'final_resource_zero_owner_unverified_samples': sum(
                1 for d in final if all(_is_int(d.get(k)) and d[k] == 0
                                        for k in ZERO_RESOURCE_KEYS) and
                not _owner_telemetry_complete(d)),
            'baseline_resource_zero_owner_unhealthy_samples': sum(
                1 for d in baseline if all(_is_int(d.get(k)) and d[k] == 0
                                          for k in ZERO_RESOURCE_KEYS) and
                _owner_telemetry_complete(d) and not _steady_memory_data(d, model)),
            'final_resource_zero_owner_unhealthy_samples': sum(
                1 for d in final if all(_is_int(d.get(k)) and d[k] == 0
                                        for k in ZERO_RESOURCE_KEYS) and
                _owner_telemetry_complete(d) and not _steady_memory_data(d, model)),
            'median_free_memory_loss_bytes': loss,
            'coarse_1024_byte_threshold_pass': bool(threshold_pass),
            'leak_free_proven': False,
            'memory_uptime': uptime_result,
        }
    checks['memory_uptime'] = all(memory_uptime_results.values())
    checks['memory_and_owner'] = all(memory_results.values())

    return {
        'audit_schema': 1,
        'label': label,
        'candidate_version': version,
        'evidence_run_id': run.get('run_id'),
        'completion_audit_pass': not reasons,
        'release_decision': 'not_assessed',
        'checks': checks,
        'phase_results': phase_results,
        'scenario_results': scenario_results,
        'recovery_results': recovery_results,
        'http_counts': http_counts,
        'illegal_http_rows': illegal_http_rows,
        'unexpected_http_200_rows': unexpected_200_rows,
        'uptime_monotonic_by_target': uptime_ok,
        'memory_uptime_monotonic_by_target': memory_uptime_results,
        'memory_by_target': memory_counts,
        'control_evidence_boundary': (
            'Full control-state equality is enforced by Runner.check and represented here by '
            'a completed run with no blockers; evidence rows do not retain complete per-sample controls. '
            'Final configuration equality is represented by completion_checks.configuration_unchanged.'
        ),
        'heap_evidence_boundary': (
            'The 1024-byte comparison uses at least three resource-zero, healthy-owner samples in both '
            'baseline and final, and is a coarse threshold only. Non-steady samples remain counted. '
            'Idle zero-allocation and owner counters are bounded observations; this audit does not '
            'prove absence of leaks.'
        ),
        'reasons': sorted(set(reasons)),
    }


def audit_directory(run_dir, label, version, output=None):
    run_dir = Path(run_dir)
    evidence = json.loads((run_dir / 'evidence.json').read_text())
    plan = json.loads((run_dir / 'run-plan.json').read_text())
    process_exit = json.loads((run_dir / 'process-exit.json').read_text())
    source_path = run_dir / 'runner-source.py'
    source_bytes = source_path.read_bytes() if source_path.exists() else None
    result = _audit(evidence, plan, process_exit, source_bytes, label, version)
    target = Path(output) if output else run_dir / 'independent-completion-audit.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(result, indent=2, sort_keys=True)+'\n')
    target.chmod(0o600)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', required=True, type=Path)
    parser.add_argument('--label', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    try:
        result = audit_directory(args.run_dir, args.label, args.version, args.output)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(json.dumps({'completion_audit_pass': False,
                          'release_decision': 'not_assessed',
                          'reasons': ['input_unreadable_or_malformed:'+type(exc).__name__]}, sort_keys=True))
        return 2
    # Console output is allowlisted; never include device state or config.
    print(json.dumps({key: result[key] for key in (
        'label', 'candidate_version', 'completion_audit_pass', 'release_decision',
        'checks', 'reasons')}, indent=2, sort_keys=True))
    return 0 if result['completion_audit_pass'] else 2


if __name__ == '__main__':
    sys.exit(main())
