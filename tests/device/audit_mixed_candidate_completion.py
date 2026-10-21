#!/usr/bin/env python3
"""Offline completion audit for the final mixed HTTP/MQTT acceptance run.

This validates recorded evidence. A passing audit is not release approval and
does not establish leak freedom.
"""
import argparse
import base64
import hashlib
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/status101'
PRIVATE = BASE / 'private'
TARGETS = {
    '10.117.152.111': ('b0f893303134', 'a1'),
    '10.117.152.102': ('D0BAE463E12C', 'tc1'),
    '10.117.152.110': ('b0f8932864b6', 'm1'),
}
HTTP_PHASES = {
    'concurrency-1': (1, 600), 'concurrency-4': (4, 600),
    'concurrency-8': (8, 120), 'concurrency-16': (16, 120), 'soak': (4, 60),
}
HTTP_SCENARIOS = ('concurrency-1', 'concurrency-4', 'concurrency-8', 'concurrency-16',
                  'slow-clients', 'aborted-clients', 'buffered-read-clients',
                  'missing-read-clients', 'soak')
HTTP_RECOVERIES = tuple(name+'-recovery' for name in HTTP_PHASES) + (
    'incomplete-headers-recovery', 'aborted-connections-recovery',
    'buffered-read-input-recovery', 'missing-read-eof-recovery')
MIXED_SHORT_PLAN = (
    {'name': 'mixed-1', 'http_clients': 1, 'mqtt_window': 1, 'seconds': 60},
    {'name': 'mixed-4', 'http_clients': 4, 'mqtt_window': 2, 'seconds': 120},
    {'name': 'mixed-8', 'http_clients': 8, 'mqtt_window': 4, 'seconds': 60},
    {'name': 'mixed-16', 'http_clients': 16, 'mqtt_window': 4, 'seconds': 60},
)
MIXED_SOAK_PLAN = ({'name': 'mixed-soak', 'http_clients': 4, 'mqtt_window': 4,
                   'seconds': 14400, 'mqtt_period': 1.0},)
ZERO_FIELDS = ('json_live_blocks', 'memory_active', 'memory_reserved', 'memory_waiting')
OWNER_FIELDS = ('http_core_owner_state', 'http_core_close_failures', 'http_core_restarts',
                'http_core_tcp_limit', 'http_core_tcp_rc', 'http_core_occupied',
                'http_core_server_bytes', 'http_tcp_sample_ms')
QUEUE_ZERO_FIELDS = ('mqtt_rx_queued', 'mqtt_tx_queued', 'mqtt_inflight',
                     'mqtt_queue_bytes', 'mqtt_credit_bytes')
QUEUE_REQUIRED_FIELDS = QUEUE_ZERO_FIELDS + (
    'mqtt_peak_bytes', 'mqtt_owner_faults', 'mqtt_generation', 'mqtt_rejected',
    'mqtt_rx_buffer_bytes', 'mqtt_tx_buffer_bytes', 'mqtt_protocol_depth',
    'mqtt_operation', 'mqtt_last_error_stage', 'mqtt_last_error', 'mqtt_errors',
    'mqtt_reconnects', 'mqtt_max_queue_delay_ms')
READ_ACTIONS = {'status.get', 'capabilities.get', 'logs.get', 'config.get',
                'power.get', 'binding.status.get'}
MQTT_OPERATION_MAX = 7
MQTT_BUFFER_MAX = 12288
REQUIRED_ACCEPTANCE_SOURCES = {
    'controller.py', 'http_status_schema_samples.py', 'http_mqtt_schema_parity.py',
    'audit_mixed_candidate_completion.py',
}


def _read_json(path):
    value = json.loads(Path(path).read_text())
    if not isinstance(value, dict):
        raise ValueError('Evidence root must be an object')
    return value


def _sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _require(condition, code, failures):
    if not condition:
        failures.append(code)


def _identity_gate(runner, version, failures, stage):
    before = runner.get('before')
    ok = isinstance(before, dict) and set(before) == set(TARGETS)
    if ok:
        for ip, (ident, model) in TARGETS.items():
            item = before[ip] if isinstance(before[ip], dict) else {}
            state = item.get('state') if isinstance(item.get('state'), dict) else {}
            ok &= str(item.get('id', '')).lower() == ident.lower()
            ok &= str(state.get('device_id', '')).lower() == ident.lower()
            ok &= state.get('model') == model and state.get('version') == version
            ok &= isinstance(item.get('config'), dict)
            keys = {'a1': ('requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled'),
                    'tc1': ('socket_states', 'child_lock', 'led_enabled'),
                    'm1': ('brightness_requested',)}[model]
            ok &= all(key in state for key in keys)
    _require(bool(ok), stage+'_identity_model_version_config_controls', failures)


def _runner_gate(runner, stage, failures):
    run = runner.get('run') if isinstance(runner.get('run'), dict) else {}
    _require(run.get('status') == 'complete' and run.get('completed') is True and
             run.get('failure_type') is None, stage+'_runner_not_complete', failures)
    _require(runner.get('blockers') == [], stage+'_runner_blockers', failures)
    checks = runner.get('completion_checks') if isinstance(runner.get('completion_checks'), dict) else {}
    _require(all(checks.get(name) is True for name in
                 ('network_restore', 'configuration_unchanged', 'idle_quiescence')),
             stage+'_network_config_idle_gates', failures)
    _uptime_gate(runner, stage, failures)


def _uptime_gate(runner, stage, failures):
    before = runner.get('before') if isinstance(runner.get('before'), dict) else {}
    rows = runner.get('rows') if isinstance(runner.get('rows'), list) else []
    all_ok = True
    for ip in TARGETS:
        base = before.get(ip) if isinstance(before.get(ip), dict) else {}
        state = base.get('state') if isinstance(base.get('state'), dict) else {}
        seed_start = base.get('status_request_started_monotonic')
        seed_end = base.get('status_request_ended_monotonic')
        seed_uptime = state.get('uptime_seconds')
        origin = base.get('boot_origin')
        if not all(type(value) in (int, float) and math.isfinite(value)
                   for value in (seed_start, seed_end, seed_uptime, origin)):
            all_ok = False
            continue
        samples = [(seed_start, seed_end, seed_uptime)]
        for row in rows:
            if (not isinstance(row, dict) or row.get('ip') != ip or row.get('action') != 'status.get' or
                    row.get('ok') is not True):
                continue
            start, end, uptime = (row.get('request_started_monotonic'),
                                  row.get('request_ended_monotonic'), row.get('uptime'))
            if not all(type(value) in (int, float) and math.isfinite(value)
                       for value in (start, end, uptime)) or end < start or uptime < 0:
                all_ok = False
                continue
            if start-uptime > origin+5:
                all_ok = False
            samples.append((start, end, uptime))
        ordered = sorted(samples, key=lambda row: (row[0], row[1]))
        for left, right in zip(ordered, ordered[1:]):
            if left[1] <= right[0] and right[2] < left[2]:
                all_ok = False
        memory = runner.get('memory') if isinstance(runner.get('memory'), list) else []
        memory_samples = [(seed_start, seed_end, seed_uptime*1000)]
        for row in memory:
            if not isinstance(row, dict) or row.get('ip') != ip or row.get('ok') is not True:
                continue
            data = row.get('data') if isinstance(row.get('data'), dict) else {}
            uptime_ms = data.get('uptime_ms')
            start, end = row.get('request_started_monotonic'), row.get('request_ended_monotonic')
            if not all(type(value) in (int, float) and math.isfinite(value)
                       for value in (start, end, uptime_ms)) or end < start or uptime_ms < 0:
                all_ok = False
                continue
            if start-uptime_ms/1000 > origin+5:
                all_ok = False
            memory_samples.append((start, end, uptime_ms))
        memory_samples.sort(key=lambda row: (row[0], row[1]))
        for left, right in zip(memory_samples, memory_samples[1:]):
            if left[1] <= right[0] and right[2] < left[2]:
                all_ok = False
    _require(all_ok, stage+'_uptime_continuity', failures)


def _phase_records(evidence, expected, name_key, clients_key, seconds_key, failures, stage):
    phases = evidence.get('phases')
    phases = phases if isinstance(phases, list) else []
    ok = len(phases) == len(expected)
    for spec, phase in zip(expected, phases):
        ok &= isinstance(phase, dict)
        if not isinstance(phase, dict):
            continue
        ok &= phase.get(name_key) == spec['name'] and phase.get('completed') is True
        ok &= phase.get(clients_key) == spec[clients_key]
        ok &= phase.get(seconds_key) == spec[seconds_key]
        elapsed = phase.get('elapsed_seconds')
        ok &= type(elapsed) in (int, float) and math.isfinite(elapsed) and elapsed >= spec[seconds_key]
    _require(bool(ok), stage+'_phases_plan_duration', failures)
    return phases


def _audit_http_regression(evidence, version, failures):
    stage = 'http_regression'
    _require(evidence.get('version') == version, stage+'_version', failures)
    _runner_gate(evidence, stage, failures)
    _identity_gate(evidence, version, failures, stage)
    run = evidence.get('run') if isinstance(evidence.get('run'), dict) else {}
    plan = run.get('plan') if isinstance(run.get('plan'), dict) else {}
    _require(plan.get('mode') == 'full', stage+'_not_full_plan', failures)
    _require(plan.get('normal_phase_seconds') == 600 and plan.get('phase_seconds') == 120 and
             plan.get('soak_seconds') == 60 and plan.get('soak_clients') == 4 and
             plan.get('soak_period') == 1.0, stage+'_plan_settings', failures)
    expected = [{'name': name, 'clients': clients, 'seconds': seconds}
                for name, (clients, seconds) in HTTP_PHASES.items()]
    phases = evidence.get('phases') if isinstance(evidence.get('phases'), list) else []
    phase_map = {row.get('name'): row for row in phases if isinstance(row, dict)}
    phase_ok = len(phases) == len(expected)
    for spec in expected:
        row = phase_map.get(spec['name'], {})
        phase_ok &= (row.get('completed') is True and
                     row.get('clients_per_device') == spec['clients'] and
                     row.get('requested_seconds') == spec['seconds'] and
                     type(row.get('elapsed_seconds')) in (int, float) and
                     row.get('elapsed_seconds', 0) >= spec['seconds'])
    _require(bool(phase_ok), stage+'_phase_plan', failures)
    scenarios = evidence.get('scenarios') if isinstance(evidence.get('scenarios'), list) else []
    scenario_ok = all(sum(isinstance(row, dict) and row.get('name') == name and
                          row.get('completed') is True for row in scenarios) == 1
                      for name in HTTP_SCENARIOS)
    _require(scenario_ok, stage+'_scenario_completion', failures)
    recovery_ok = True
    recoveries = evidence.get('recoveries') if isinstance(evidence.get('recoveries'), list) else []
    for name in HTTP_RECOVERIES:
        matches = [row for row in recoveries if isinstance(row, dict) and row.get('name') == name]
        covered = set()
        for record in matches:
            targets = record.get('targets')
            if record.get('completed') is not True or not isinstance(targets, dict):
                recovery_ok = False
                continue
            for ip, result in targets.items():
                if (ip not in TARGETS or ip in covered or not isinstance(result, dict) or
                        result.get('completed') is not True or result.get('consecutive_successes', 0) < 3):
                    recovery_ok = False
                covered.add(ip)
        recovery_ok &= covered == set(TARGETS)
    _require(recovery_ok, stage+'_per_target_recovery', failures)
    rows = evidence.get('rows') if isinstance(evidence.get('rows'), list) else []
    actions_ok = bool(rows)
    for row in rows:
        if not isinstance(row, dict) or row.get('ip') not in TARGETS or row.get('action') not in READ_ACTIONS:
            actions_ok = False
            continue
        status = row.get('http')
        if status not in (None, 200, 503):
            actions_ok = False
        if status == 200 and row.get('code') not in ('ok', 'busy', 'invalid_params'):
            actions_ok = False
    _require(actions_ok, stage+'_read_only_http_results', failures)
    _audit_load_diagnostics(evidence, failures, stage)


def _quiet_resources(runner, version, failures, stage, require_queue):
    memory = runner.get('memory') if isinstance(runner.get('memory'), list) else []
    per_target = {}
    metrics_available = True
    for ip, (ident, model) in TARGETS.items():
        rows = [row for row in memory if isinstance(row, dict) and row.get('ip') == ip and
                row.get('phase') == 'final' and row.get('ok') is True and row.get('http') == 200 and
                isinstance(row.get('data'), dict)]
        tail = rows[-3:]
        ok = len(tail) == 3
        diagnostics = [row['data'] for row in tail]
        if ok:
            for data in diagnostics:
                ok &= all(type(data.get(key)) is int and data[key] == 0 for key in ZERO_FIELDS)
                ok &= type(data.get('http_core_owner_state')) is int and data['http_core_owner_state'] == 1
                ok &= type(data.get('http_core_close_failures')) is int and data['http_core_close_failures'] == 0
                ok &= type(data.get('http_core_restarts')) is int and data['http_core_restarts'] == 0
                ok &= type(data.get('http_core_tcp_rc')) is int and data['http_core_tcp_rc'] == 0
                occupied = data.get('http_core_occupied')
                ok &= type(occupied) is int and 0 <= occupied <= 1
        queue_by_row = []
        for data in diagnostics:
            present = all(key in data for key in QUEUE_REQUIRED_FIELDS)
            metrics_available &= present
            if not present:
                queue_by_row.append(None)
                continue
            unsigned = tuple(key for key in QUEUE_REQUIRED_FIELDS if key != 'mqtt_last_error')
            typed = (all(type(data.get(key)) is int and data[key] >= 0 for key in unsigned) and
                     type(data.get('mqtt_last_error')) is int and
                     data['mqtt_protocol_depth'] <= 1 and
                     data['mqtt_operation'] <= MQTT_OPERATION_MAX and
                     data['mqtt_last_error_stage'] <= MQTT_OPERATION_MAX and
                     data['mqtt_rx_buffer_bytes'] <= MQTT_BUFFER_MAX and
                     data['mqtt_tx_buffer_bytes'] <= MQTT_BUFFER_MAX)
            queue_by_row.append({key: data[key] for key in QUEUE_REQUIRED_FIELDS} if typed else None)
            ok &= typed
            if typed:
                ok &= all(data[key] == 0 for key in QUEUE_ZERO_FIELDS)
                ok &= data['mqtt_owner_faults'] == 0
                ok &= data['mqtt_peak_bytes'] <= 12288
                ok &= data['mqtt_queue_bytes'] <= 12288
        if require_queue:
            ok &= all(row is not None for row in queue_by_row)
        baseline_rows = [row for row in memory if isinstance(row, dict) and row.get('ip') == ip and
                         row.get('phase') == 'baseline' and row.get('ok') is True and
                         row.get('http') == 200 and isinstance(row.get('data'), dict)]
        baseline_tail = baseline_rows[-3:]
        native_baseline = None
        native_final = None
        baseline_diag = [{key: row['data'].get(key) for key in QUEUE_REQUIRED_FIELDS
                          if key in row['data']} for row in baseline_tail]
        final_diag = [{key: row.get(key) for key in QUEUE_REQUIRED_FIELDS
                       if key in row} for row in diagnostics]
        error_delta = reconnect_delta = None
        if (len(baseline_tail) == 3 and len(diagnostics) == 3 and
                all(type(row['data'].get(key)) is int for row in baseline_tail
                    for key in ('mqtt_errors', 'mqtt_reconnects')) and
                all(type(row.get(key)) is int for row in diagnostics
                    for key in ('mqtt_errors', 'mqtt_reconnects'))):
            error_delta = diagnostics[-1]['mqtt_errors'] - baseline_tail[-1]['data']['mqtt_errors']
            reconnect_delta = diagnostics[-1]['mqtt_reconnects'] - baseline_tail[-1]['data']['mqtt_reconnects']
        if (baseline_tail and len(baseline_tail) == 3 and
                all(all(key in row['data'] for key in ('mqtt_rx_buffer_bytes', 'mqtt_tx_buffer_bytes'))
                    for row in baseline_tail) and len(diagnostics) == 3 and
                all(isinstance(row, dict) and 'mqtt_rx_buffer_bytes' in row and
                    'mqtt_tx_buffer_bytes' in row for row in diagnostics)):
            native_baseline = [max(row['data']['mqtt_rx_buffer_bytes'] for row in baseline_tail),
                               max(row['data']['mqtt_tx_buffer_bytes'] for row in baseline_tail)]
            native_final = [max(row['mqtt_rx_buffer_bytes'] for row in diagnostics),
                            max(row['mqtt_tx_buffer_bytes'] for row in diagnostics)]
            ok &= all(type(value) is int and value >= 0 for value in native_baseline+native_final)
            if all(type(value) is int for value in native_baseline+native_final):
                ok &= native_final[0] <= native_baseline[0] and native_final[1] <= native_baseline[1]
        elif require_queue:
            ok = False
        per_target[ip] = {
            'model': model, 'identity': ident, 'terminal_samples': len(tail),
            'queue_metrics': 'available' if all(row is not None for row in queue_by_row) and tail else 'NOT_AVAILABLE',
            'native_buffer_baseline_max': native_baseline,
            'native_buffer_final_max': native_final,
            'native_buffer_assessment': (
                'NOT_AVAILABLE' if native_baseline is None or native_final is None else
                'nonincreasing' if native_final[0] <= native_baseline[0] and
                native_final[1] <= native_baseline[1] else
                'increased_but_bounded_investigate'),
            'baseline_diagnostics': baseline_diag,
            'terminal_diagnostics': final_diag,
            'mqtt_errors_delta': error_delta,
            'mqtt_reconnects_delta': reconnect_delta,
            'native_buffer_nonincreasing': (native_baseline is not None and native_final is not None and
                                            native_final[0] <= native_baseline[0] and
                                            native_final[1] <= native_baseline[1]),
            'passed': bool(ok),
        }
        _require(bool(ok), stage+'_quiet_resources:'+ip, failures)
    if require_queue:
        _require(metrics_available, stage+'_mqtt_diagnostics_missing', failures)
    return per_target


def _audit_load_diagnostics(runner, failures, stage):
    memory = runner.get('memory') if isinstance(runner.get('memory'), list) else []
    output = {}
    fields = ('mqtt_rx_queued', 'mqtt_tx_queued', 'mqtt_inflight', 'mqtt_queue_bytes',
              'mqtt_peak_bytes', 'mqtt_credit_bytes', 'mqtt_owner_faults',
              'mqtt_rx_buffer_bytes', 'mqtt_tx_buffer_bytes', 'mqtt_errors',
              'mqtt_reconnects', 'mqtt_last_error_stage', 'mqtt_last_error',
              'mqtt_operation', 'mqtt_protocol_depth', 'mqtt_max_queue_delay_ms')
    for ip in TARGETS:
        rows = [row for row in memory if isinstance(row, dict) and row.get('ip') == ip and
                isinstance(row.get('data'), dict)]
        available = []
        violations = []
        by_phase = {}
        for row in rows:
            data = row['data']
            present = {key: data[key] for key in fields if key in data}
            if not present:
                continue
            valid = {key: value for key, value in present.items()
                     if type(value) is int and value >= 0}
            signed_error = data.get('mqtt_last_error')
            if 'mqtt_last_error' in present and type(signed_error) is int:
                valid['mqtt_last_error'] = signed_error
            available.append({'phase': row.get('phase'), 'at': row.get('at'),
                              'fields': valid})
            phase = str(row.get('phase', 'unknown'))
            stats = by_phase.setdefault(phase, {'samples': 0, 'available_samples': 0,
                                                'violations': 0, 'maximum_queue_bytes': None,
                                                'maximum_peak_bytes': None,
                                                'max_native_buffers': [None, None],
                                                'mqtt_errors_first': None,
                                                'mqtt_errors_last': None,
                                                'mqtt_errors_delta': None,
                                                'mqtt_reconnects_first': None,
                                                'mqtt_reconnects_last': None,
                                                'mqtt_reconnects_delta': None})
            stats['samples'] += 1
            if present:
                stats['available_samples'] += 1
            bad = False
            for key in present:
                if key == 'mqtt_last_error':
                    malformed = type(present[key]) is not int
                else:
                    malformed = key not in valid
                if malformed:
                    bad = True
            if 'mqtt_protocol_depth' in valid:
                bad |= valid['mqtt_protocol_depth'] > 1
            if 'mqtt_operation' in valid:
                bad |= valid['mqtt_operation'] > MQTT_OPERATION_MAX
            if 'mqtt_last_error_stage' in valid:
                bad |= valid['mqtt_last_error_stage'] > MQTT_OPERATION_MAX
            if 'mqtt_queue_bytes' in valid and 'mqtt_credit_bytes' in valid:
                bad |= valid['mqtt_queue_bytes']+valid['mqtt_credit_bytes'] > 12288
            if 'mqtt_peak_bytes' in valid:
                bad |= valid['mqtt_peak_bytes'] > 12288
                stats['maximum_peak_bytes'] = max(stats['maximum_peak_bytes'] or 0,
                                                  valid['mqtt_peak_bytes'])
            if 'mqtt_queue_bytes' in valid:
                stats['maximum_queue_bytes'] = max(stats['maximum_queue_bytes'] or 0,
                                                   valid['mqtt_queue_bytes'])
            if 'mqtt_rx_queued' in valid and 'mqtt_inflight' in valid:
                bad |= valid['mqtt_rx_queued']+valid['mqtt_inflight'] > 4
            if 'mqtt_tx_queued' in valid and 'mqtt_inflight' in valid:
                bad |= valid['mqtt_tx_queued']+valid['mqtt_inflight'] > 10
            if 'mqtt_rx_queued' in valid:
                bad |= valid['mqtt_rx_queued'] > 4
            if 'mqtt_tx_queued' in valid:
                bad |= valid['mqtt_tx_queued'] > 6
            if 'mqtt_owner_faults' in valid:
                bad |= valid['mqtt_owner_faults'] != 0
            if 'mqtt_rx_buffer_bytes' in valid:
                stats['max_native_buffers'][0] = max(stats['max_native_buffers'][0] or 0,
                                                     valid['mqtt_rx_buffer_bytes'])
                bad |= valid['mqtt_rx_buffer_bytes'] > MQTT_BUFFER_MAX
            if 'mqtt_tx_buffer_bytes' in valid:
                stats['max_native_buffers'][1] = max(stats['max_native_buffers'][1] or 0,
                                                     valid['mqtt_tx_buffer_bytes'])
                bad |= valid['mqtt_tx_buffer_bytes'] > MQTT_BUFFER_MAX
            for key in ('mqtt_errors', 'mqtt_reconnects'):
                if key in valid:
                    if stats[key+'_first'] is None:
                        stats[key+'_first'] = valid[key]
                    stats[key+'_last'] = valid[key]
                    stats[key+'_delta'] = stats[key+'_last']-stats[key+'_first']
            stats['violations'] += int(bad)
            if bad:
                violations.append(row.get('phase'))
        _require(not violations, stage+'_load_diagnostic_bounds:'+ip, failures)
        field_status = {key: ('available' if any(key in row.get('fields', {}) for row in available)
                              else 'NOT_AVAILABLE') for key in fields}
        output[ip] = {'availability': 'available' if available else 'NOT_AVAILABLE',
                      'samples': len(rows), 'diagnostic_samples': len(available),
                      'fields': field_status, 'phases': by_phase,
                      'violating_phases': violations}
    return output


def _audit_mixed(evidence, runner_path, expected_plan, version, label, require_soak, failures, stage):
    _require(evidence.get('status') == 'complete' and evidence.get('completed') is True and
             evidence.get('blockers') == [] and evidence.get('version') == version and
             evidence.get('label') == label, stage+'_completion_version_label', failures)
    targets = evidence.get('targets') if isinstance(evidence.get('targets'), dict) else {}
    identity_ok = set(targets) == set(TARGETS)
    if identity_ok:
        for ip, (ident, model) in TARGETS.items():
            item = targets[ip] if isinstance(targets[ip], dict) else {}
            identity_ok &= str(item.get('device_id', '')).lower() == ident.lower()
            identity_ok &= item.get('model') == model
    _require(bool(identity_ok), stage+'_target_identity', failures)
    expected = list(expected_plan)
    plan = evidence.get('plan') if isinstance(evidence.get('plan'), list) else []
    phases = evidence.get('phases') if isinstance(evidence.get('phases'), list) else []
    plan_ok = plan == expected and len(phases) == len(expected)
    for spec, phase in zip(expected, phases):
        if not isinstance(phase, dict):
            plan_ok = False
            continue
        plan_ok &= all(phase.get(key) == value for key, value in spec.items())
        elapsed = phase.get('elapsed_seconds')
        plan_ok &= phase.get('completed') is True and type(elapsed) in (int, float) and elapsed >= spec['seconds']
        if require_soak:
            plan_ok &= (phase.get('telemetry_observation') is None or
                        isinstance(phase.get('telemetry_observation'), dict))
    _require(bool(plan_ok), stage+'_phase_plan_duration', failures)
    recoveries = evidence.get('recovery') if isinstance(evidence.get('recovery'), list) else []
    recovery_ok = len(recoveries) == len(expected)
    for phase, recovery in zip(expected, recoveries):
        recovery_ok &= isinstance(recovery, dict) and recovery.get('phase') == phase['name']
        recovery_ok &= recovery.get('completed') is True and recovery.get('http_completed') is True
        rows = recovery.get('mqtt_results') if isinstance(recovery.get('mqtt_results'), list) else []
        recovery_ok &= len(rows) == len(TARGETS)
        recovery_ok &= {row.get('ip') for row in rows if isinstance(row, dict)} == set(TARGETS)
        recovery_ok &= all(isinstance(row, dict) and row.get('outcome') == 'success' and
                           row.get('retained') is False for row in rows)
    _require(bool(recovery_ok), stage+'_per_target_recovery', failures)
    runner_path = Path(runner_path)
    runner = _read_json(runner_path) if runner_path.is_file() else None
    if runner is None:
        failures.append(stage+'_http_runner_evidence_missing')
    else:
        _runner_gate(runner, stage, failures)
        _identity_gate(runner, version, failures, stage+'_http')
        _audit_load_diagnostics(runner, failures, stage+'_http')
    return runner


def _audit_readonly_mqtt(evidence, version, failures):
    expected = {
        '10.117.152.111': ('b0f893303134', 'a1'),
        '10.117.152.102': ('D0BAE463E12C', 'tc1'),
        '10.117.152.110': ('b0f8932864b6', 'm1'),
    }
    requests = evidence.get('requests') if isinstance(evidence.get('requests'), list) else []
    ok = (evidence.get('status') == 'complete' and evidence.get('completed') is True and
          evidence.get('version') == version and evidence.get('http_prechecks') == 'passed' and
          evidence.get('http_postchecks') == 'passed' and evidence.get('blockers') == [] and
          len(requests) == 9)
    seen_ids = set()
    modes = {ip: [] for ip in expected}
    for row in requests:
        if not isinstance(row, dict):
            ok = False
            continue
        ip = row.get('ip')
        if ip not in expected:
            ok = False
            continue
        ident, model = expected[ip]
        request_id = row.get('request_id')
        view = row.get('mode')
        params = {} if view == 'full' else {'view': 'live'} if view == 'live' else None
        ok &= (isinstance(request_id, str) and request_id.startswith('readonly-mqtt-') and
               request_id not in seen_ids and row.get('action') == 'status.get' and
               row.get('params') == params and row.get('retained') is False and
               row.get('http_status') == 200 and row.get('response_code') == 'ok' and
               isinstance(row.get('response_device_id'), str) and
               row['response_device_id'].lower() == ident.lower() and
               row.get('response_model') == model and row.get('response_version') == version)
        if isinstance(request_id, str):
            seen_ids.add(request_id)
        modes[ip].append(view)
    ok &= all(value == ['full', 'live', 'full'] for value in modes.values())
    _require(bool(ok), 'mqtt_readonly_contract', failures)
    return {'passed': bool(ok), 'requests': len(requests),
            'per_model_views': {model: modes[ip] for ip, (_, model) in expected.items()},
            'unique_request_ids': len(seen_ids)}


def _audit_http_body_evidence(evidence, version, failures, runner=None):
    expected = TARGETS
    rows = evidence.get('samples') if isinstance(evidence, dict) else None
    rows = rows if isinstance(rows, list) else []
    ok = (evidence.get('schema') == 'candidate-http-status-bodies-v1' and
          evidence.get('status') == 'complete' and evidence.get('completed') is True and
          evidence.get('version') == version and len(rows) == 6 and
          evidence.get('blockers') == [])
    before = runner.get('before') if isinstance(runner, dict) else None
    ok &= isinstance(before, dict) and set(before) == set(expected)
    targets = [{'ip': ip, 'device_id': ident, 'model': model}
               for ip, (ident, model) in expected.items()]
    ok &= evidence.get('targets') == targets
    seen_ids = set()
    views = {ip: [] for ip in expected}
    valid_samples = 0
    for row in rows:
        if not isinstance(row, dict):
            ok = False
            continue
        ip = row.get('ip')
        if ip not in expected:
            ok = False
            continue
        ident, model = expected[ip]
        view = row.get('view')
        params = {} if view == 'full' else {'view': 'live'} if view == 'live' else None
        request_id = row.get('request_id')
        encoded = row.get('raw_body_base64')
        if (not isinstance(request_id, str) or not request_id.startswith('readonly-http-schema-') or
                request_id in seen_ids or row.get('model') != model or
                row.get('action') != 'status.get' or row.get('params') != params or
                row.get('http_status') != 200 or row.get('response_code') != 'ok' or
                row.get('response_device_id') != ident or row.get('response_model') != model or
                row.get('response_version') != version or not isinstance(encoded, str)):
            ok = False
            continue
        seen_ids.add(request_id)
        views[ip].append(view)
        try:
            raw = base64.b64decode(encoded, validate=True)
            envelope = json.loads(raw.decode('utf-8'))
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError):
            ok = False
            continue
        data = envelope.get('data') if isinstance(envelope, dict) else None
        identity = data.get('device_id', data.get('id')) if isinstance(data, dict) else None
        base = before.get(ip) if isinstance(before, dict) else None
        base_state = base.get('state') if isinstance(base, dict) else None
        controls = {'a1': ('requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled'),
                    'tc1': ('socket_states', 'child_lock', 'led_enabled'),
                    'm1': ('brightness_requested',)}[model]
        control_match = (isinstance(base_state, dict) and isinstance(data, dict) and
                         all(key in base_state and key in data and data[key] == base_state[key]
                             for key in controls))
        connectivity_match = (isinstance(base_state, dict) and isinstance(data, dict) and
                              all(data.get(key) == base_state[key]
                                  for key in ('wifi_connected', 'mqtt_connected')
                                  if key in base_state))
        uptime = data.get('uptime_seconds') if isinstance(data, dict) else None
        start = row.get('request_started_monotonic')
        end = row.get('request_ended_monotonic')
        origin = base.get('boot_origin') if isinstance(base, dict) else None
        timing_valid = (all(type(value) in (int, float) and math.isfinite(value)
                            for value in (uptime, start, end, origin)) and
                        end >= start and uptime >= 0 and start-uptime <= origin+5)
        if (len(raw) > 65536 or row.get('body_bytes') != len(raw) or
                not isinstance(envelope, dict) or envelope.get('code') != 'ok' or
                not isinstance(data, dict) or not isinstance(identity, str) or
                identity.lower() != ident.lower() or data.get('model') != model or
                data.get('version') != version or not control_match or not connectivity_match or
                data.get('watchdog_state') not in (None, 'healthy', 'starting') or not timing_valid):
            ok = False
            continue
        valid_samples += 1
    ok &= all(views[ip] == ['full', 'live'] for ip in expected)
    ok &= valid_samples == 6 and len(seen_ids) == 6
    for ip in expected:
        target_samples = []
        for row in rows:
            if not isinstance(row, dict) or row.get('ip') != ip:
                continue
            encoded = row.get('raw_body_base64')
            try:
                envelope = json.loads(base64.b64decode(encoded, validate=True).decode('utf-8'))
                data = envelope.get('data', {})
                start = row.get('request_started_monotonic')
                end = row.get('request_ended_monotonic')
                uptime = data.get('uptime_seconds')
                if (all(type(value) in (int, float) and math.isfinite(value)
                        for value in (start, end, uptime)) and end >= start):
                    target_samples.append((start, end, uptime))
            except (ValueError, UnicodeDecodeError, json.JSONDecodeError, TypeError, AttributeError):
                continue
        target_samples.sort(key=lambda item: item[0] if type(item[0]) in (int, float) else -1)
        for left, right in zip(target_samples, target_samples[1:]):
            left_start, left_end, left_uptime = left
            right_start, right_end, right_uptime = right
            if not all(type(value) in (int, float) and math.isfinite(value)
                       for value in (left_start, left_end, left_uptime,
                                     right_start, right_end, right_uptime)):
                ok = False
                continue
            if (left_end >= left_start and right_end >= right_start and
                    left_end <= right_start and right_uptime < left_uptime):
                ok = False
    _require(bool(ok), 'http_schema_body_contract', failures)
    return {'passed': bool(ok), 'samples': len(rows), 'validated_bodies': valid_samples,
            'per_target_views': {model: views[ip] for ip, (_, model) in expected.items()},
            'unique_request_ids': len(seen_ids)}


def _audit_schema_parity(report, version, failures):
    contract = report.get('final_contract') if isinstance(report, dict) else None
    contract = contract if isinstance(contract, dict) else {}
    candidate = contract.get('candidate') if isinstance(contract.get('candidate'), dict) else {}
    sources = contract.get('sources') if isinstance(contract.get('sources'), dict) else {}
    candidate_mqtt = sources.get('candidate_mqtt') if isinstance(sources.get('candidate_mqtt'), dict) else {}
    candidate_http = sources.get('candidate_http_full') if isinstance(sources.get('candidate_http_full'), dict) else {}
    candidate_http_live = sources.get('candidate_http_live') if isinstance(sources.get('candidate_http_live'), dict) else {}
    issues = contract.get('integrity_issues') if isinstance(contract.get('integrity_issues'), list) else []
    candidate_issues = [row for row in issues if isinstance(row, dict) and
                        row.get('source') in ('candidate_http', 'candidate_mqtt')]
    http_counts = candidate_http.get('sample_counts')
    http_live_counts = candidate_http_live.get('sample_counts')
    mqtt_groups = candidate_mqtt.get('groups') if isinstance(candidate_mqtt.get('groups'), dict) else {}
    counts_ok = http_counts == {'a1': 1, 'tc1': 1, 'm1': 1}
    counts_ok &= http_live_counts == {'a1': 1, 'tc1': 1, 'm1': 1}
    for model in ('a1', 'tc1', 'm1'):
        group = mqtt_groups.get(model) if isinstance(mqtt_groups.get(model), dict) else {}
        counts_ok &= group.get('full', {}).get('response_count') == 2
        counts_ok &= group.get('live', {}).get('response_count') == 1
    ok = (contract.get('schema') == 'http-mqtt-final-contract-v1' and
          candidate.get('expected_version') == version and candidate.get('version_match') is True and
          candidate.get('all_response_versions_match') is True and not candidate_issues and
          candidate_mqtt.get('issues') == [] and counts_ok)
    _require(bool(ok), 'http_mqtt_schema_parity_integrity', failures)
    comparisons = contract.get('comparisons') if isinstance(contract.get('comparisons'), dict) else {}
    parity = comparisons.get('candidate_http_full_vs_candidate_mqtt_full', {})
    live_parity = comparisons.get('candidate_http_live_vs_candidate_mqtt_live', {})
    historical_live = comparisons.get('historical_http_live_vs_candidate_http_live', {})
    parity_groups_ok = all(
        isinstance(parity.get(f'{model}/full'), dict) and
        parity[f'{model}/full'].get('status') == 'compared' and
        isinstance(live_parity.get(f'{model}/live'), dict) and
        live_parity[f'{model}/live'].get('status') == 'compared'
        for model in ('a1', 'tc1', 'm1'))
    historical_gap_ok = all(
        isinstance(historical_live.get(f'{model}/live'), dict) and
        historical_live[f'{model}/live'].get('status') == 'unavailable_no_historical_http_live_body'
        for model in ('a1', 'tc1', 'm1'))
    ok &= parity_groups_ok and historical_gap_ok
    _require(parity_groups_ok, 'candidate_http_mqtt_view_parity_missing', failures)
    _require(historical_gap_ok, 'historical_http_live_gap_not_reported', failures)
    return {'passed': bool(ok), 'candidate_issue_count': len(candidate_issues),
            'schema_difference_count': len(parity.get('differences', []))
            if isinstance(parity, dict) and isinstance(parity.get('differences'), list) else None,
            'live_schema_difference_count': len(live_parity.get('differences', []))
            if isinstance(live_parity, dict) and isinstance(live_parity.get('differences'), list) else None,
            'historical_http_live': 'unavailable_no_historical_http_live_body',
            'differences_blocking': False}


def _verify_hash_records(output, records, failures):
    if not isinstance(records, dict) or not records:
        failures.append('source_manifest_missing')
        return {'source_unchanged': False, 'source_copies_unchanged': False}
    source_ok = copies_ok = True
    source_hashes, copy_hashes = {}, {}
    for name, item in records.items():
        if not isinstance(item, dict):
            source_ok = copies_ok = False
            continue
        try:
            source_hashes[name] = _sha256(item['source_path'])
        except (OSError, KeyError, TypeError):
            source_hashes[name] = None
        try:
            copy_hashes[name] = _sha256(output/item['archive_path'])
        except (OSError, KeyError, TypeError):
            copy_hashes[name] = None
        source_ok &= source_hashes[name] == item.get('sha256')
        copies_ok &= copy_hashes[name] == item.get('archived_sha256')
    _require(source_ok, 'source_changed', failures)
    _require(copies_ok, 'source_archive_changed', failures)
    return {'source_unchanged': bool(source_ok), 'source_copies_unchanged': bool(copies_ok),
            'source_sha256_at_audit': source_hashes,
            'archived_sha256_at_audit': copy_hashes}


def _require_acceptance_sources(records, failures):
    names = set(records) if isinstance(records, dict) else set()
    missing = sorted(REQUIRED_ACCEPTANCE_SOURCES - names)
    _require(not missing, 'required_acceptance_sources_missing', failures)
    return missing


def _contains_pair(argv, key, value):
    return any(argv[index:index+2] == [key, value] for index in range(len(argv)-1))


def _audit_stage_argv(stage_map, label, archive, commit, version, failures):
    contracts = {
        'initial': ('run_initial_shared_screen.py',
                    (('--archive', archive), ('--commit', commit), ('--version', version),
                     ('--label', label)), ('--execute',)),
        'pre_stability': ('validate_shared_candidate.py',
                          (('--archive', archive), ('--version', version),
                           ('--label', label+'-pre')), ('--existing-runtime',
                           '--pre-stability-only', '--execute')),
        'mqtt_readonly': ('readonly_mqtt_candidate.py',
                          (('--version', version), ('--label', label+'-mqtt-readonly')),
                          ('--execute',)),
        'http_schema_samples': ('http_status_schema_samples.py',
                                (('--version', version),
                                 ('--label', label+'-http-schema-bodies')),
                                ('--execute',)),
        'schema_parity': ('http_mqtt_schema_parity.py',
                          (('--contract-version', version),), ()),
        'http_regression': ('readonly_stress',
                            (('--version', version), ('--normal-phase-seconds', '600'),
                             ('--phase-seconds', '120'), ('--soak-seconds', '60'),
                             ('--soak-clients', '4'), ('--soak-period', '1')),
                            ('--require-shared-core',)),
        'mixed_short': ('mqtt_mixed_controller.py',
                        (('--label', label+'-mixed-short'), ('--version', version)),
                        ('--execute',)),
        'mixed_soak': ('mqtt_http_mixed.py',
                       (('--version', version), ('--label', label+'-mixed-soak'),
                        ('--profile', 'soak')), ('--execute',)),
        'mixed_postcheck': ('mixed_final_postcheck.py',
                            (('--label', label), ('--version', version)), ('--execute',)),
    }
    targets = [item for ip, (ident, _model) in TARGETS.items() for item in (ip+'='+ident,)]
    for name, (executable, pairs, flags) in contracts.items():
        row = stage_map.get(name, {})
        argv = row.get('argv') if isinstance(row.get('argv'), list) else []
        ok = any(executable in str(arg) for arg in argv)
        ok &= all(_contains_pair(argv, key, value) for key, value in pairs)
        ok &= all(flag in argv for flag in flags)
        if name in ('mqtt_readonly', 'http_schema_samples', 'http_regression', 'mixed_soak'):
            found = [argv[index+1] for index, item in enumerate(argv[:-1]) if item == '--target']
            ok &= found == targets
        if name == 'schema_parity':
            ok &= _contains_pair(argv, '--contract-http-evidence',
                                 str(PRIVATE/(label+'-pre-pre-stability-idle')/'evidence.json'))
            ok &= _contains_pair(argv, '--contract-mqtt-evidence',
                                 str(PRIVATE/(label+'-mqtt-readonly')/'mqtt-evidence.json'))
            ok &= _contains_pair(argv, '--candidate-http-body-evidence',
                                 str(PRIVATE/(label+'-http-schema-bodies')/'http-bodies.json'))
            ok &= _contains_pair(argv, '--candidate-runner-manifest',
                                 str(PRIVATE/(label+'-mixed-final-controller')/'source-manifest.json'))
            ok &= _contains_pair(argv, '--output',
                                 str(PRIVATE/(label+'-http-mqtt-schema-contract.json')))
        if name == 'mqtt_readonly':
            ok &= _contains_pair(argv, '--output',
                                 str(PRIVATE/(label+'-mqtt-readonly')))
        if name == 'http_schema_samples':
            ok &= _contains_pair(argv, '--output',
                                 str(PRIVATE/(label+'-http-schema-bodies')))
        if name == 'http_regression':
            ok &= '-m' in argv and 'tests.device.readonly_stress' in argv
        if name == 'mixed_soak':
            ok &= _contains_pair(argv, '--output', str(PRIVATE/(label+'-mixed-soak')))
        if name == 'mixed_postcheck':
            ok &= _contains_pair(argv, '--run-dir', str(PRIVATE/(label+'-mixed-soak')))
        _require(bool(ok), name+'_stage_argv_contract', failures)


def audit_controller(controller_path, label, archive, commit, version):
    controller_path = Path(controller_path)
    controller_dir = controller_path.parent
    controller = _read_json(controller_path)
    failures = []
    _require(controller.get('label') == label and controller.get('archive') == archive and
             controller.get('commit') == commit and controller.get('version') == version,
             'controller_candidate_identity_mismatch', failures)
    stages = controller.get('stages') if isinstance(controller.get('stages'), list) else []
    expected_names = ['initial', 'pre_stability', 'mqtt_readonly', 'http_schema_samples', 'schema_parity',
                      'http_regression', 'mixed_short', 'mixed_soak', 'mixed_postcheck']
    _require([row.get('name') for row in stages if isinstance(row, dict)] == expected_names,
             'stage_sequence_incomplete', failures)
    stage_map = {row.get('name'): row for row in stages if isinstance(row, dict)}
    process_ok = len(stages) == len(expected_names)
    for name in expected_names:
        row = stage_map.get(name, {})
        process_ok &= row.get('status') == 'complete' and row.get('exit_code') == 0
        process_ok &= type(row.get('pid')) is int and row['pid'] > 0
        process_ok &= row.get('process_group_empty') is True
        process_ok &= row.get('unexpected_descendants') is False
        process_ok &= row.get('descendant_cleanup_attempted') is False
        process_ok &= row.get('timed_out') is False and row.get('failure_evidence') is False
        process_ok &= row.get('interrupted') is False
    _require(bool(process_ok), 'stage_process_completion_or_cleanup', failures)
    _audit_stage_argv(stage_map, label, archive, commit, version, failures)
    sources = controller.get('sources')
    _require_acceptance_sources(sources, failures)
    source_audit = _verify_hash_records(controller_dir, sources, failures)
    candidate_audit = _verify_hash_records(controller_dir, controller.get('candidate_files'), failures)
    source_manifest_path = controller_dir/'source-manifest.json'
    try:
        source_manifest = _read_json(source_manifest_path)
        source_manifest_ok = (source_manifest.get('sources') == controller.get('sources') and
                              source_manifest.get('validation_commit') == controller.get('validation_commit') and
                              _sha256(source_manifest_path) == controller.get('source_manifest_sha256'))
    except (OSError, ValueError, TypeError):
        source_manifest_ok = False
    _require(source_manifest_ok, 'source_manifest_integrity', failures)
    _require(bool(controller.get('tracked_worktree_clean')), 'tracked_worktree_not_clean', failures)
    _require(controller.get('validation_commit') == controller.get('commit') or
             controller.get('validation_paths_are_tests_docs') is True,
             'candidate_validation_tree_drift', failures)
    _require(controller.get('validation_paths_are_tests_docs') is True,
             'candidate_validation_drift_gate_missing', failures)
    _require(controller.get('validation_tree_unchanged') is True,
             'validation_tree_changed_during_run', failures)

    # The child artifacts are located by fixed controller-owned stage paths.
    initial_path = Path(stage_map.get('initial', {}).get('evidence_path', ''))
    pre_path = Path(stage_map.get('pre_stability', {}).get('evidence_path', ''))
    mqtt_path = Path(stage_map.get('mqtt_readonly', {}).get('evidence_path', ''))
    http_body_path = Path(stage_map.get('http_schema_samples', {}).get('evidence_path', ''))
    schema_path = Path(stage_map.get('schema_parity', {}).get('evidence_path', ''))
    http_path = Path(stage_map.get('http_regression', {}).get('evidence_path', ''))
    short_path = Path(stage_map.get('mixed_short', {}).get('evidence_path', ''))
    soak_path = Path(stage_map.get('mixed_soak', {}).get('evidence_path', ''))
    post_path = Path(stage_map.get('mixed_postcheck', {}).get('evidence_path', ''))
    initial = _read_json(initial_path) if initial_path.is_file() else {}
    pre = _read_json(pre_path) if pre_path.is_file() else {}
    mqtt = _read_json(mqtt_path) if mqtt_path.is_file() else {}
    http_body = _read_json(http_body_path) if http_body_path.is_file() else {}
    http_body_runner_path = http_body_path.parent/'evidence.json'
    http_body_runner = _read_json(http_body_runner_path) if http_body_runner_path.is_file() else {}
    schema = _read_json(schema_path) if schema_path.is_file() else {}
    http = _read_json(http_path) if http_path.is_file() else {}
    short_controller = _read_json(short_path) if short_path.is_file() else {}
    short_evidence_path = Path(short_controller.get('output', ''))/'mixed-evidence.json'
    short = _read_json(short_evidence_path) if short_evidence_path.is_file() else {}
    soak = _read_json(soak_path) if soak_path.is_file() else {}
    post = _read_json(post_path) if post_path.is_file() else {}

    initial_steps = initial.get('steps') if isinstance(initial.get('steps'), list) else []
    initial_process_ok = (len(initial_steps) == 4 and
                          all(isinstance(row, dict) and row.get('status') == 'complete' and
                              type(row.get('pid')) is int and row['pid'] > 0 and
                              row.get('exit_code') == 0 for row in initial_steps))
    initial_ok = (initial.get('status') == 'complete' and initial.get('completed') is True and
                  initial.get('initial_signed_ota_complete') is True and
                  initial.get('paced_diagnostic_complete') is True and
                  initial.get('firmware_commit', initial.get('commit')) == commit and
                  initial.get('version') == version and initial.get('source_unchanged') is True and
                  initial.get('source_copies_unchanged') is True and
                  initial.get('source_manifest_unchanged') is True and
                  type(initial.get('pid')) is int and initial['pid'] > 0 and
                  initial.get('exit_code') == 0 and initial_process_ok)
    _require(initial_ok, 'initial_signed_ota_or_paced_gate', failures)
    _require(initial.get('targets') == [ip+'='+ident for ip, (ident, _) in TARGETS.items()],
             'initial_targets_mismatch', failures)
    _require(pre.get('run', {}).get('status') == 'complete' and
             pre.get('run', {}).get('completed') is True and pre.get('blockers') == [],
             'pre_stability_only_runner_incomplete', failures)
    _identity_gate(pre, version, failures, 'pre_stability')
    _runner_gate(pre, 'pre_stability', failures)
    pre_checks = pre.get('completion_checks') if isinstance(pre.get('completion_checks'), dict) else {}
    _require(pre_checks.get('idle_quiescence') is True, 'pre_stability_idle_gate', failures)
    mqtt_readonly_summary = _audit_readonly_mqtt(mqtt, version, failures)
    http_body_summary = _audit_http_body_evidence(http_body, version, failures, http_body_runner)
    _runner_gate(http_body_runner, 'http_schema_samples', failures)
    _identity_gate(http_body_runner, version, failures, 'http_schema_samples')
    schema_parity_summary = _audit_schema_parity(schema, version, failures)
    _audit_http_regression(http, version, failures)
    _require(short_controller.get('status') == 'complete' and short_controller.get('completed') is True and
             short_controller.get('source_unchanged') is True and
             short_controller.get('source_copies_unchanged') is True and
             short_controller.get('process', {}).get('exit_code') == 0 and
             short_controller.get('process', {}).get('timed_out') is False and
             short_controller.get('process', {}).get('terminated_after_failure_evidence') is False,
             'mixed_short_controller_incomplete', failures)
    short_runner_path = Path(short_controller.get('output', ''))/'http-runner/evidence.json'
    short_runner = _audit_mixed(short, short_runner_path, MIXED_SHORT_PLAN, version,
                                label+'-mixed-short', False, failures, 'mixed_short')
    if short_runner is not None:
        _quiet_resources(short_runner, version, failures, 'mixed_short', require_queue=False)
        short_load_summary = _audit_load_diagnostics(short_runner, failures, 'mixed_short')
    else:
        short_load_summary = {}
    soak_runner_path = soak_path.parent/'http-runner/evidence.json'
    soak_runner = _audit_mixed(soak, soak_runner_path, MIXED_SOAK_PLAN, version,
                               label+'-mixed-soak', True, failures, 'mixed_soak')
    if soak_runner is not None:
        quiet_summary = _quiet_resources(soak_runner, version, failures, 'mixed_soak', require_queue=True)
        soak_load_summary = _audit_load_diagnostics(soak_runner, failures, 'mixed_soak')
    else:
        quiet_summary = {}
        soak_load_summary = {}
    _require(post.get('status') == 'complete' and post.get('completed') is True and
             post.get('source_unchanged') is True and post.get('source_copies_unchanged') is True and
             post.get('source_inputs_unchanged') is True and post.get('runner_completed') is True and
             post.get('exit_code') == 0 and post.get('process_group_empty') is True and
             post.get('process_group_cleanup_confirmed') is True,
             'mixed_postcheck_incomplete', failures)
    output = {
        'schema': 1, 'label': label, 'archive': archive, 'commit': commit, 'version': version,
        'completion_audit_pass': not failures, 'failures': failures,
        'stages': {name: stage_map.get(name, {}).get('status', 'missing') for name in expected_names},
        'source_audit': source_audit, 'candidate_package_audit': candidate_audit,
        'initial_signed_ota_complete': initial_ok,
        'pre_stability_only_complete': pre.get('run', {}).get('completed') is True,
        'mqtt_readonly_complete': mqtt_readonly_summary['passed'],
        'mqtt_readonly_summary': mqtt_readonly_summary,
        'http_schema_samples_complete': http_body_summary['passed'] and
                                        http_body_runner.get('run', {}).get('completed') is True,
        'http_schema_samples_summary': http_body_summary,
        'schema_parity_candidate_integrity': schema_parity_summary,
        'http_regression_complete': not any(item.startswith('http_regression_') for item in failures),
        'mixed_short_complete': short.get('completed') is True,
        'http_load_diagnostics': _audit_load_diagnostics(http, [], 'http_regression_report'),
        'mixed_short_load_diagnostics': short_load_summary,
        'mixed_soak_load_diagnostics': soak_load_summary,
        'mixed_soak_complete': soak.get('completed') is True,
        'mixed_postcheck_complete': post.get('completed') is True,
        'mixed_soak_quiet_resources': quiet_summary,
        'interpretation': 'Evidence audit only; not release approval and not proof of leak freedom',
    }
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--controller', type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--archive', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    report = audit_controller(args.controller, args.label, args.archive, args.commit, args.version)
    if args.output:
        temp = args.output.with_suffix(args.output.suffix+'.tmp')
        temp.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        temp.write_text(json.dumps(report, indent=2, sort_keys=True)+'\n')
        temp.chmod(0o600)
        temp.replace(args.output)
        args.output.chmod(0o600)
    print(json.dumps({key: report[key] for key in
                      ('label', 'archive', 'commit', 'version', 'completion_audit_pass', 'failures')},
                     sort_keys=True))
    return 0 if report['completion_audit_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
