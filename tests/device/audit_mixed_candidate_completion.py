#!/usr/bin/env python3
"""Offline completion audit for the final mixed HTTP/MQTT acceptance run.

This validates recorded evidence. A passing audit is not release approval and
does not establish leak freedom.
"""
import argparse
import base64
import copy
import hashlib
import json
import math
import re
from pathlib import Path

if __package__:
    from . import accepted_upload_postcheck as ACCEPTED_UPLOAD
else:
    import accepted_upload_postcheck as ACCEPTED_UPLOAD

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
MQTT_OPERATION_MAX = 14
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


def _target_scope(targets=None):
    if targets is None:
        return dict(TARGETS)
    if isinstance(targets, (tuple, list)):
        parsed = {}
        for spec in targets:
            if not isinstance(spec, str) or spec.count('=') != 1:
                raise ValueError('Malformed audit target')
            ip, identity = spec.split('=', 1)
            if ip in parsed:
                raise ValueError('Duplicate audit target')
            parsed[ip] = identity
        targets = parsed
    if not isinstance(targets, dict):
        raise ValueError('Audit target scope must be a mapping or target list')
    result = {}
    for ip, value in targets.items():
        if ip not in TARGETS:
            raise ValueError('Unknown target in audit scope')
        if isinstance(value, (tuple, list)) and len(value) == 2:
            identity, model = value
            if (str(identity).lower() != TARGETS[ip][0].lower() or
                    model != TARGETS[ip][1]):
                raise ValueError('Target identity/model differs from authorization')
        else:
            identity, model = TARGETS[ip]
            if str(value).lower() != identity.lower():
                raise ValueError('Target identity differs from authorization')
        result[ip] = TARGETS[ip]
    if not result or ('10.117.152.110' in result and set(result) != set(TARGETS)):
        raise ValueError('Audit target scope must be a nonempty authorized subset')
    return {ip: result[ip] for ip in TARGETS if ip in result}


def _identity_gate(runner, version, failures, stage, targets=None):
    selected = _target_scope(targets)
    before = runner.get('before')
    ok = isinstance(before, dict) and set(before) == set(selected)
    if ok:
        for ip, (ident, model) in selected.items():
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


def _runner_gate(runner, stage, failures, targets=None):
    selected = _target_scope(targets)
    run = runner.get('run') if isinstance(runner.get('run'), dict) else {}
    _require(run.get('status') == 'complete' and run.get('completed') is True and
             run.get('failure_type') is None, stage+'_runner_not_complete', failures)
    _require(runner.get('blockers') == [], stage+'_runner_blockers', failures)
    _require(_scope_records_ok(runner, selected), stage+'_runner_unselected_device_records', failures)
    checks = runner.get('completion_checks') if isinstance(runner.get('completion_checks'), dict) else {}
    _require(all(checks.get(name) is True for name in
                 ('network_restore', 'configuration_unchanged', 'idle_quiescence')),
             stage+'_network_config_idle_gates', failures)
    _uptime_gate(runner, stage, failures, targets)


def _uptime_gate(runner, stage, failures, targets=None):
    selected = _target_scope(targets)
    before = runner.get('before') if isinstance(runner.get('before'), dict) else {}
    rows = runner.get('rows') if isinstance(runner.get('rows'), list) else []
    all_ok = True
    for ip in selected:
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


def _scope_records_ok(document, selected):
    """Reject device-attributed evidence for any authorized but unselected DUT."""
    selected_ips = set(selected)
    authorized_ips = set(TARGETS)
    valid = True

    def visit(value, parent_key=None):
        nonlocal valid
        if isinstance(value, dict):
            for key, child in value.items():
                if parent_key != 'device_status' and key in authorized_ips and key not in selected_ips:
                    valid = False
                if key == 'ip' and isinstance(child, str) and child in authorized_ips and child not in selected_ips:
                    valid = False
                if isinstance(key, str) and any(key.startswith(ip+' ') for ip in authorized_ips-selected_ips):
                    valid = False
                visit(child, key)
        elif isinstance(value, list):
            for child in value:
                visit(child, parent_key)

    visit(document)
    return valid


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


def _audit_http_regression(evidence, version, failures, targets=None):
    selected = _target_scope(targets)
    stage = 'http_regression'
    _require(evidence.get('version') == version, stage+'_version', failures)
    _runner_gate(evidence, stage, failures, selected)
    _identity_gate(evidence, version, failures, stage, selected)
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
                if (ip not in selected or ip in covered or not isinstance(result, dict) or
                        result.get('completed') is not True or result.get('consecutive_successes', 0) < 3):
                    recovery_ok = False
                covered.add(ip)
        recovery_ok &= covered == set(selected)
    _require(recovery_ok, stage+'_per_target_recovery', failures)
    rows = evidence.get('rows') if isinstance(evidence.get('rows'), list) else []
    actions_ok = bool(rows)
    for row in rows:
        if not isinstance(row, dict) or row.get('ip') not in selected or row.get('action') not in READ_ACTIONS:
            actions_ok = False
            continue
        status = row.get('http')
        if status not in (None, 200, 503):
            actions_ok = False
        if status == 200 and row.get('code') not in ('ok', 'busy', 'invalid_params'):
            actions_ok = False
    _require(actions_ok, stage+'_read_only_http_results', failures)
    _audit_load_diagnostics(evidence, failures, stage, selected)


def _quiet_resources(runner, version, failures, stage, require_queue, targets=None):
    selected = _target_scope(targets)
    memory = runner.get('memory') if isinstance(runner.get('memory'), list) else []
    per_target = {}
    metrics_available = True
    for ip, (ident, model) in selected.items():
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


def _audit_load_diagnostics(runner, failures, stage, targets=None):
    selected = _target_scope(targets)
    memory = runner.get('memory') if isinstance(runner.get('memory'), list) else []
    output = {}
    fields = ('mqtt_rx_queued', 'mqtt_tx_queued', 'mqtt_inflight', 'mqtt_queue_bytes',
              'mqtt_peak_bytes', 'mqtt_credit_bytes', 'mqtt_owner_faults',
              'mqtt_rx_buffer_bytes', 'mqtt_tx_buffer_bytes', 'mqtt_errors',
              'mqtt_reconnects', 'mqtt_last_error_stage', 'mqtt_last_error',
              'mqtt_operation', 'mqtt_protocol_depth', 'mqtt_max_queue_delay_ms')
    for ip in selected:
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


def _audit_mixed(evidence, runner_path, expected_plan, version, label, require_soak,
                 failures, stage, targets=None):
    selected = _target_scope(targets)
    _require(_scope_records_ok(evidence, selected), stage+'_unselected_device_records', failures)
    _require(evidence.get('status') == 'complete' and evidence.get('completed') is True and
             evidence.get('blockers') == [] and evidence.get('version') == version and
             evidence.get('label') == label, stage+'_completion_version_label', failures)
    targets = evidence.get('targets') if isinstance(evidence.get('targets'), dict) else {}
    identity_ok = set(targets) == set(selected)
    if 'selected_targets' in evidence:
        identity_ok &= evidence.get('selected_targets') == list(selected)
        identity_ok &= evidence.get('device_status') == {
            ip: ('SELECTED' if ip in selected else 'PAUSED') for ip in TARGETS}
    elif set(selected) != set(TARGETS):
        identity_ok = False
    if identity_ok:
        for ip, (ident, model) in selected.items():
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
        recovery_ok &= len(rows) == len(selected)
        recovery_ok &= {row.get('ip') for row in rows if isinstance(row, dict)} == set(selected)
        recovery_ok &= all(isinstance(row, dict) and row.get('outcome') == 'success' and
                           row.get('retained') is False for row in rows)
    _require(bool(recovery_ok), stage+'_per_target_recovery', failures)
    runner_path = Path(runner_path)
    runner = _read_json(runner_path) if runner_path.is_file() else None
    if runner is None:
        failures.append(stage+'_http_runner_evidence_missing')
    else:
        _runner_gate(runner, stage, failures, selected)
        _identity_gate(runner, version, failures, stage+'_http', selected)
        _audit_load_diagnostics(runner, failures, stage+'_http', selected)
    return runner


def _audit_readonly_mqtt(evidence, version, failures, targets=None):
    selected = _target_scope(targets)
    expected = {
        '10.117.152.111': ('b0f893303134', 'a1'),
        '10.117.152.102': ('D0BAE463E12C', 'tc1'),
        '10.117.152.110': ('b0f8932864b6', 'm1'),
    }
    expected = {ip: expected[ip] for ip in selected}
    requests = evidence.get('requests') if isinstance(evidence.get('requests'), list) else []
    selected_status_ok = (
        evidence.get('selected_targets') == list(expected) and
        evidence.get('device_status') == {
            ip: ('SELECTED' if ip in expected else 'PAUSED') for ip in TARGETS})
    legacy_full = set(expected) == set(TARGETS) and 'selected_targets' not in evidence and 'device_status' not in evidence
    ok = (evidence.get('status') == 'complete' and evidence.get('completed') is True and
          evidence.get('version') == version and evidence.get('http_prechecks') == 'passed' and
          evidence.get('http_postchecks') == 'passed' and evidence.get('blockers') == [] and
          len(requests) == 3*len(expected) and
          _scope_records_ok(evidence, expected) and
          (selected_status_ok or legacy_full))
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


def _audit_http_body_evidence(evidence, version, failures, runner=None, targets=None):
    expected = _target_scope(targets)
    rows = evidence.get('samples') if isinstance(evidence, dict) else None
    rows = rows if isinstance(rows, list) else []
    ok = (evidence.get('schema') == 'candidate-http-status-bodies-v1' and
          evidence.get('status') == 'complete' and evidence.get('completed') is True and
          evidence.get('version') == version and len(rows) == 2*len(expected) and
          evidence.get('blockers') == [] and _scope_records_ok(evidence, expected))
    before = runner.get('before') if isinstance(runner, dict) else None
    ok &= isinstance(before, dict) and set(before) == set(expected)
    if 'selected_targets' in evidence:
        ok &= evidence.get('selected_targets') == list(expected)
        ok &= evidence.get('device_status') == {
            ip: ('SELECTED' if ip in expected else 'PAUSED') for ip in TARGETS}
    elif set(expected) != set(TARGETS):
        ok = False
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
    ok &= valid_samples == 2*len(expected) and len(seen_ids) == 2*len(expected)
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


def _audit_schema_parity(report, version, failures, targets=None):
    selected = _target_scope(targets)
    _require(_scope_records_ok(report, selected), 'http_mqtt_schema_parity_unselected_device_records', failures)
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
    expected_counts = {model: 1 for _, model in selected.values()}
    counts_ok = http_counts == expected_counts
    counts_ok &= http_live_counts == expected_counts
    models = tuple(model for _, model in selected.values())
    scope_ok = (contract.get('selected_targets') == list(selected) and
                contract.get('device_status') == {
                    ip: ('SELECTED' if ip in selected else 'PAUSED') for ip in TARGETS})
    if set(selected) == set(TARGETS) and 'selected_targets' not in contract and 'device_status' not in contract:
        scope_ok = True
    for model in models:
        group = mqtt_groups.get(model) if isinstance(mqtt_groups.get(model), dict) else {}
        counts_ok &= group.get('full', {}).get('response_count') == 2
        counts_ok &= group.get('live', {}).get('response_count') == 1
    ok = (contract.get('schema') == 'http-mqtt-final-contract-v1' and
          candidate.get('expected_version') == version and candidate.get('version_match') is True and
          candidate.get('all_response_versions_match') is True and not candidate_issues and
          candidate_mqtt.get('issues') == [] and counts_ok and scope_ok)
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
        for model in models)
    historical_gap_ok = all(
        isinstance(historical_live.get(f'{model}/live'), dict) and
        historical_live[f'{model}/live'].get('status') == 'unavailable_no_historical_http_live_body'
        for model in models)
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


def _reuse_check(condition, message):
    if not condition:
        raise ValueError('Initial reuse: '+message)


def _process_complete(row, group=False):
    ok = (isinstance(row, dict) and row.get('status') == 'complete' and
          row.get('exit_code') == 0 and type(row.get('pid')) is int and row['pid'] > 0)
    if group:
        ok &= row.get('process_group_empty') is True
        ok &= all(row.get(key) is False for key in
                  ('unexpected_descendants', 'descendant_cleanup_attempted',
                   'timed_out', 'failure_evidence', 'interrupted'))
    return bool(ok)


# Only these reviewed host helper replacements may differ from the failed run.
PARTIAL_HELPER_REPLACEMENTS = {
    'controller.py': ('0036e6a490fe21161091344546c0962b2ca6c5896ee7162bf403fc3e48e1f31f', 'a631bba0bd8cb60a0b1b0c5e1f865ab2d880b6ffc1a92cdd9ce509ee73c7207e'),
    'validate_shared_candidate.py': ('b70c8221e936d12d7e8a89727dd8ab3423dbadd4f41c4188e0ee1dc7c3f20a0f', 'd627e7eff7d962d14f44131726409f1b98ca87140e79268c61966e548d120587'),
}
PARTIAL_CONTROL_FIELDS = ('socket_states', 'requested_fan_on', 'requested_speed',
                          'child_lock', 'led_enabled', 'brightness_requested', 'fan_mode',
                          'auto_max_speed', 'led_always_on', 'brightness', 'red', 'green', 'blue')


def verify_completed_deploy(label, archive, commit, version, previous):
    """Accept only the single observed successful A1 child of the known failure.

    This never changes or promotes the failed parent. All historical files and
    packages are anchored; helper replacement hashes are explicitly allowlisted.
    """
    _reuse_check(bool(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9-]{0,63}', label)), 'invalid partial label')
    inner_dir = PRIVATE/(label+'-initial-controller')
    outer_dir = PRIVATE/(label+'-mixed-final-controller')
    deploy_dir = PRIVATE/('deploy-'+label+'-initial-111')
    proofs = {}
    def read(path):
        path = Path(path).resolve(strict=True)
        _reuse_check(path.is_relative_to(BASE.resolve()), 'partial evidence outside tree')
        proofs[str(path)] = _sha256(path)
        return _read_json(path)
    inner, outer = read(inner_dir/'controller.json'), read(outer_dir/'controller.json')
    _reuse_check(inner.get('status') == outer.get('status') == 'failed' and
                 inner.get('completed') is outer.get('completed') is False and
                 inner.get('exit_code') == 1 and inner.get('failure_type') == 'TypeError' and
                 inner.get('failure') == "'NoneType' object is not iterable" and
                 inner.get('initial_signed_ota_complete') is False and
                 inner.get('paced_diagnostic_complete') is False, 'partial failure differs')
    for record, key in ((inner, 'firmware_commit'), (outer, 'commit')):
        _reuse_check(record.get('label') == label and record.get('archive') == archive and
                     record.get(key) == commit and record.get('version') == version,
                     'partial candidate differs')
    _reuse_check(inner.get('previous_version') == previous and
                 inner.get('targets') == {ip: ident for ip, (ident, _) in TARGETS.items()},
                 'partial targets/previous differ')
    _reuse_check(all(inner.get(k) is True for k in
                    ('source_unchanged', 'source_copies_unchanged', 'source_manifest_unchanged', 'tracked_worktree_clean')) and
                 all(outer.get(k) is True for k in
                    ('source_unchanged', 'source_copies_unchanged', 'candidate_files_unchanged', 'tracked_worktree_clean')),
                 'partial original source gate failed')
    steps = inner.get('steps', [])
    _reuse_check(len(steps) == 1 and _process_complete(steps[0]), 'partial must have one successful child')
    step = steps[0]
    _reuse_check(step.get('name') == label+'-initial-deploy-111' and
                 len(step.get('argv', [])) == 6 and
                 Path(step['argv'][1]).name == 'deploy_candidate.py' and
                 step['argv'][2:] == ['111', label+'-initial', previous, archive], 'partial child differs')
    for path in (BASE/(step['name']+'.log'), outer_dir/'initial.log'):
        proofs[str(path.resolve(strict=True))] = _sha256(path)
    stages = outer.get('stages', [])
    _reuse_check(len(stages) == 1 and stages[0].get('name') == 'initial' and
                 stages[0].get('pid') == inner.get('pid') and stages[0].get('exit_code') == 1 and
                 stages[0].get('status') == 'failed' and stages[0].get('process_group_empty') is True and
                 not any(stages[0].get(k) for k in ('unexpected_descendants', 'descendant_cleanup_attempted',
                                                    'timed_out', 'interrupted')),
                 'failed parent child observation differs')
    _reuse_check(set(deploy_dir.iterdir()) == {deploy_dir/(name+'.json') for name in
                    ('before', 'upload-intent', 'upload', 'after')}, 'partial deploy files differ')
    _reuse_check(set(PRIVATE.glob('deploy-'+label+'-initial-*/upload-intent.json')) ==
                 {deploy_dir/'upload-intent.json'}, 'another upload intent exists')
    before, intent, upload, after = (read(deploy_dir/(name+'.json')) for name in
                                    ('before', 'upload-intent', 'upload', 'after'))
    _reuse_check(step.get('started_at', 0) <= intent.get('at', -1) <= step.get('ended_at', 0),
                 'partial upload intent outside observed child')
    _reuse_check(upload.get('http') == 202 and json.loads(upload.get('body', '{}')) ==
                 {'message': 'OTA verified; rebooting'}, 'partial upload not accepted')
    ip = '10.117.152.111'
    ident, model = TARGETS[ip]
    for state, wanted in ((before.get('status', {}), previous), (after.get('status', {}), version)):
        _reuse_check(state.get('device_id', '').lower() == ident.lower() and state.get('model') == model and
                     state.get('version') == wanted, 'partial runtime identity/version differs')
    _reuse_check(before.get('config') == after.get('config') and isinstance(after.get('config'), dict) and
                 after['status']['uptime_seconds'] < before['status']['uptime_seconds'],
                 'partial config/reboot differs')
    for key in PARTIAL_CONTROL_FIELDS:
        _reuse_check(before['status'].get(key) == after['status'].get(key), 'partial controls differ:'+key)
    changes = {}
    for folder, record in ((inner_dir, inner), (outer_dir, outer)):
        manifest_path = folder/'source-manifest.json'
        manifest = read(manifest_path)
        _reuse_check(_sha256(manifest_path) == record.get('source_manifest_sha256'), 'partial source manifest changed')
        required = {'controller.py', 'readonly_stress.py', 'deploy_candidate.py',
                    'diagnose_shared_candidate.py', 'ota_package.py', 'status_benchmark.py',
                    'validate_shared_candidate.py'}
        _reuse_check(required <= set(manifest.get('sources', {})), 'partial required sources missing')
        if folder == inner_dir:
            _reuse_check(manifest.get('candidate_commit') == commit and manifest.get('candidate_version') == version,
                         'partial archived candidate identity differs')
        else:
            _reuse_check(manifest.get('sources') == outer.get('sources'), 'partial outer source records differ')
        for name, item in manifest['sources'].items():
            path = (folder/item['archive_path']).resolve(strict=True)
            _reuse_check(path.is_relative_to(folder.resolve()), 'partial archive escape')
            digest = _sha256(path)
            proofs[str(path)] = digest
            _reuse_check(digest == item['sha256'] == item['archived_sha256'], 'partial source archive changed')
            if folder == inner_dir:
                current = ROOT/item['path']
                current_digest = _sha256(current)
                if current_digest != digest:
                    _reuse_check(name in PARTIAL_HELPER_REPLACEMENTS and
                                 (digest, current_digest) == PARTIAL_HELPER_REPLACEMENTS[name],
                                 'unapproved partial helper change:'+name)
                    changes[name] = {'old_sha256': digest, 'new_sha256': current_digest}
                elif name in PARTIAL_HELPER_REPLACEMENTS:
                    _reuse_check(False, 'required helper repair missing:'+name)
    expected_files = {model+'/'+filename for model in ('a1', 'tc1', 'm1')
                      for filename in ('manifest.json', 'SHA256SUMS',
                      'phicomm_'+('tc1_a1' if model == 'tc1' else model)+'.ota.bin',
                      'phicomm_'+('tc1_a1' if model == 'tc1' else model)+'.ota.bin.signed')}
    _reuse_check(set(outer.get('candidate_files', {})) == expected_files, 'partial candidate archive incomplete')
    for item in outer['candidate_files'].values():
        path = (outer_dir/item['archive_path']).resolve(strict=True)
        _reuse_check(path.is_relative_to(outer_dir.resolve()) and
                     _sha256(path) == item['sha256'] == item['archived_sha256'] == _sha256(item['source_path']),
                     'partial candidate archive changed')
        proofs[str(path)] = _sha256(path)
        proofs[str(Path(item['source_path']).resolve())] = _sha256(item['source_path'])
    _reuse_check(set(changes) == set(PARTIAL_HELPER_REPLACEMENTS), 'helper repair set differs')
    from tools.ota.package import verify_package
    folder = BASE/'candidates'/archive/'a1'
    manifest = read(folder/'manifest.json')
    signed_path, raw_path = folder/'phicomm_a1.ota.bin.signed', folder/'phicomm_a1.ota.bin'
    signed, raw = signed_path.read_bytes(), raw_path.read_bytes()
    for path in (signed_path, raw_path):
        proofs[str(path.resolve())] = _sha256(path)
    metadata = verify_package(signed, 'a1')
    _reuse_check(intent.get('ip') == ip and intent.get('device_id', '').lower() == ident.lower() and
                 intent.get('sha256') == hashlib.sha256(signed).hexdigest() and intent.get('manifest') == manifest and
                 manifest.get('git_commit') == commit and manifest.get('version') == version and
                 manifest.get('dirty') is False and manifest.get('target') == 'A1@MK3080B@moc' and
                 metadata.get('model') == 'a1' and metadata.get('commit') == commit and
                 metadata.get('version') == manifest.get('base_version') and signed[192:] == raw and
                 hashlib.sha256(raw).hexdigest() == manifest.get('files', {}).get('phicomm_a1.ota.bin'),
                 'partial signed package differs')
    return {'schema': 'completed-single-deploy-v1', 'source_label': label, 'ip': ip,
            'archive': archive, 'commit': commit, 'version': version, 'previous_version': previous,
            'original_step': step, 'original_outer_status': 'failed', 'original_inner_status': 'failed',
            'after': after, 'helper_changes': changes, 'proof_sha256': dict(sorted(proofs.items()))}


def verify_completed_deploy_fresh(proof, samples):
    """Three guarded reads retain uptime/config/control; SDK clock may run fast."""
    _reuse_check(len(samples) == 3, 'partial fresh sample count')
    ip, old = proof['ip'], proof['after']['status']
    ident, model = TARGETS[ip]
    last_uptime = old.get('uptime_seconds')
    last_upper = None
    wall_upper = proof['original_step']['ended_at']-last_uptime
    last_end = None
    for sample in samples:
        state = sample.get('status', {})
        _reuse_check(state.get('device_id', '').lower() == ident.lower() and state.get('model') == model and
                     state.get('version') == proof['version'] and sample.get('config') == proof['after']['config'],
                     'partial fresh identity/version/config differs')
        for key in PARTIAL_CONTROL_FIELDS:
            _reuse_check(state.get(key) == old.get(key), 'partial fresh controls differ:'+key)
        uptime = state.get('uptime_seconds')
        start, end = sample.get('started_monotonic'), sample.get('ended_monotonic')
        _reuse_check(type(uptime) in (int, float) and uptime >= last_uptime and
                     type(start) in (int, float) and type(end) in (int, float) and end >= start,
                     'partial fresh uptime/timing invalid')
        wall_start, wall_end = sample.get('started_at'), sample.get('ended_at')
        _reuse_check(type(wall_start) in (int, float) and type(wall_end) in (int, float) and
                     wall_end >= wall_start and wall_start-uptime-1 <= wall_upper and
                     (last_end is None or start >= last_end), 'partial fresh historical boot boundary')
        wall_upper = wall_end-uptime
        last_end = end
        lower, upper = start-uptime-1, end-uptime
        _reuse_check(last_upper is None or lower <= last_upper, 'partial fresh reboot boundary')
        last_uptime, last_upper = uptime, upper
    return {'passed': True, 'samples': samples, 'basis': 'uptime nondecreasing and adjacent boot upper bound; SDK fast clock allowed'}


def verify_accepted_upload_fresh(proof, samples):
    """Independent postcheck anchors continuity; original failed child stays failed."""
    expected = ACCEPTED_UPLOAD.verify_accepted_upload_postcheck(
        proof['cleanup_file'], root=ROOT, validation_commit=proof['validation_commit'], check_current=False)
    _reuse_check(proof == expected, 'accepted upload provenance differs')
    for sample in samples:
        ACCEPTED_UPLOAD._state(sample.get('status', {}), sample.get('config'), proof['after'])
        _reuse_check(sample.get('started_at', 0) >= proof['postcheck_step']['ended_at'],
                     'accepted upload fresh sample predates independent postcheck')
    return dict(verify_completed_deploy_fresh(proof, samples), schema='accepted-upload-fresh-v1')


def _accepted_reuse_cli(proof, argv):
    flags = {'completed-single-deploy-v1': '--reuse-completed-deploy',
             ACCEPTED_UPLOAD.SCHEMA: '--reuse-accepted-upload-postcheck'}
    flag = flags.get(proof.get('schema'))
    return (flag is not None and argv.count(flag) == 1 and
            all(other == flag or other not in argv for other in flags.values()) and
            _contains_pair(argv, flag, proof.get('source_label')))


def _initial_previous_versions(initial, targets=None):
    """Exact per-device old versions, independently of the new candidate version.

    JSON objects cannot retain duplicate keys; CLI parsers must reject repeated
    --previous IP=VERSION entries before constructing this optional mapping.
    """
    selected = _target_scope(targets)
    scalar = initial.get('previous_version')
    if 'previous_versions' not in initial:
        return {ip: scalar for ip in selected}
    mapping = initial['previous_versions']
    valid_version = lambda value: isinstance(value, str) and bool(
        re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9][A-Za-z0-9._-]{0,63})?', value))
    _reuse_check(isinstance(mapping, dict) and bool(mapping) and set(mapping) <= set(selected) and
                 valid_version(scalar) and all(valid_version(value) for value in mapping.values()),
                 'invalid initial previous_versions')
    return {ip: mapping.get(ip, scalar) for ip in selected}


def _initial_previous_cli(initial, argv, targets=None):
    if 'previous_versions' not in initial:
        return True
    try:
        _reuse_check(argv.count('--previous-version') == 1, 'missing/duplicate scalar previous version')
        scalar_at = argv.index('--previous-version')
        _reuse_check(argv[scalar_at+1] == initial['previous_version'], 'scalar previous argv differs')
        overrides = {}
        for index, arg in enumerate(argv):
            if arg != '--previous':
                continue
            ip, value = argv[index+1].split('=', 1)
            _reuse_check(ip not in overrides, 'duplicate previous override')
            overrides[ip] = value
        selected = _target_scope(targets)
        resolved = _initial_previous_versions(dict(previous_version=initial['previous_version'],
                                                   previous_versions=overrides), selected) if overrides else {
                                                       ip: initial['previous_version'] for ip in selected}
        return resolved == _initial_previous_versions(initial, selected)
    except (ValueError, KeyError, TypeError, IndexError):
        return False


def _initial_step_argv(step, index, deploy_ips, label, archive, version, previous,
                       targets=None, allow_legacy_full=False):
    selected = _target_scope(targets)
    args = step.get('argv', [])
    if index < len(deploy_ips):
        ip = deploy_ips[index]
        return (len(args) == 6 and Path(str(args[1])).name == 'deploy_candidate.py' and
                args[2:] == [ip.rsplit('.', 1)[1], label+'-initial', previous[ip], archive])
    target_args = ([] if allow_legacy_full else
                   [item for ip, (ident, _model) in selected.items()
                    for item in ('--target', ip+'='+ident)])
    return (index == len(deploy_ips) and
            Path(str(args[1])).name == 'diagnose_shared_candidate.py' and
            args[2:] == ['--version', version, '--label', label, '--execute', *target_args])


def _initial_gate(initial, commit, version, targets=None):
    steps = initial.get('steps', [])
    reused = initial.get('reused_deployments', [])
    partial_ok = False
    try:
        selected = _target_scope(targets if targets is not None else
                                 ({ip: TARGETS[ip] for ip in initial['selected_targets']}
                                  if 'selected_targets' in initial else None))
        legacy_full = 'selected_targets' not in initial and set(selected) == set(TARGETS)
        previous = _initial_previous_versions(initial, selected)
        deploy_ips = tuple(ip for ip in selected if ip != tuple(selected)[0]) if reused else tuple(selected)
        if reused and 'previous_versions' in initial:
            _reuse_check(previous[tuple(selected)[0]] == version, 'reused device previous version differs')
        _reuse_check(all(_initial_step_argv(step, index, deploy_ips, initial['label'], initial['archive'],
                                           version, previous, selected, legacy_full)
                          for index, step in enumerate(steps)),
                     'initial actual argv differs')
        _reuse_check(initial.get('targets') == {ip: ident for ip, (ident, _model) in selected.items()},
                     'initial selected target map differs')
        if 'selected_targets' in initial:
            _reuse_check(initial.get('selected_targets') == list(selected) and
                         initial.get('device_status') == {
                             ip: ('SELECTED' if ip in selected else 'PAUSED') for ip in TARGETS},
                         'initial selected/paused device status differs')
        elif set(selected) != set(TARGETS):
            _reuse_check(False, 'legacy initial evidence cannot claim a subset scope')
    except (ValueError, KeyError, TypeError):
        return False
    if reused:
        try:
            _reuse_check(len(reused) == 1 and isinstance(reused[0], dict), 'partial reuse count')
            proof = dict(reused[0])
            fresh = proof.pop('fresh_continuity')
            if proof.get('schema') == 'completed-single-deploy-v1':
                expected = verify_completed_deploy(proof['source_label'], initial['archive'], commit, version,
                                                   initial['previous_version'])
                fresh_expected = verify_completed_deploy_fresh(proof, fresh['samples'])
            elif proof.get('schema') == ACCEPTED_UPLOAD.SCHEMA:
                _reuse_check((proof.get('archive'), proof.get('commit'), proof.get('version'),
                              proof.get('previous_version')) ==
                             (initial['archive'], commit, version, initial['previous_version']),
                             'accepted upload candidate differs')
                expected = ACCEPTED_UPLOAD.verify_accepted_upload_postcheck(
                    proof['cleanup_file'], root=ROOT, validation_commit=initial['validation_commit'],
                    check_current=False)
                fresh_expected = verify_accepted_upload_fresh(proof, fresh['samples'])
            else:
                _reuse_check(False, 'unknown completed deployment schema')
            _reuse_check(proof == expected, 'partial provenance differs')
            _reuse_check(fresh == fresh_expected, 'partial fresh proof differs')
            _reuse_check(all(initial['started_at'] <= sample['started_at'] <= sample['ended_at'] <=
                             initial['ended_at'] for sample in fresh['samples']),
                         'partial fresh reads outside new execution')
            _reuse_check(len(steps) == len(selected), 'partial actual child count')
            for index, step in enumerate(steps):
                valid = _initial_step_argv(step, index, deploy_ips, initial['label'], initial['archive'],
                                           version, previous, selected, legacy_full)
                _reuse_check(valid and step['ended_monotonic'] >= step['started_monotonic'] and
                             step['ended_at'] >= step['started_at'], 'partial actual argv/timing differs')
            partial_ok = True
        except (ValueError, KeyError, TypeError, OSError):
            return False
    return (initial.get('status') == 'complete' and initial.get('completed') is True and
            initial.get('initial_signed_ota_complete') is True and
            initial.get('paced_diagnostic_complete') is True and
            initial.get('firmware_commit', initial.get('commit')) == commit and
            initial.get('version') == version and
            all(initial.get(key) is True for key in
                ('source_unchanged', 'source_copies_unchanged', 'source_manifest_unchanged')) and
            type(initial.get('pid')) is int and initial['pid'] > 0 and
            initial.get('exit_code') == 0 and
            len(steps) == len(selected) + (0 if partial_ok else 1) and
            all(_process_complete(row) for row in steps) and
            len(steps) == len(selected) + (0 if partial_ok else 1))


def verify_reusable_initial(label, archive, commit, version):
    """Re-read the immutable original evidence; never promote its outer result.

    Old archived sources are checked against their original manifest, not against
    today's test sources. Returned proofs anchor every file used by this decision.
    Hashes detect later changes; these local records are not signed attestations.
    """
    _reuse_check(bool(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9-]{0,63}', label)), 'invalid label')
    _reuse_check(bool(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9-]{0,63}', archive)), 'invalid archive')
    outer_dir = PRIVATE/(label+'-mixed-final-controller')
    inner_dir = PRIVATE/(label+'-initial-controller')
    paced_path = PRIVATE/(label+'-paced-diagnostics')/'evidence.json'
    proofs = {}

    def anchor(path):
        path = Path(path).resolve(strict=True)
        _reuse_check(path.is_relative_to(BASE.resolve()), 'evidence outside acceptance tree')
        proofs[str(path)] = _sha256(path)
        return path

    def read(path):
        return _read_json(anchor(path))

    def archived(folder, records, current=False):
        _reuse_check(isinstance(records, dict) and bool(records), 'missing archived records')
        for item in records.values():
            _reuse_check(isinstance(item, dict), 'invalid archived record')
            path = (folder/item['archive_path']).resolve(strict=True)
            _reuse_check(path.is_relative_to(folder.resolve()), 'archive path escapes original run')
            _reuse_check(_sha256(anchor(path)) == item.get('sha256') ==
                        item.get('archived_sha256'), 'archived hash mismatch')
            if current:
                source = Path(item['source_path']).resolve(strict=True)
                _reuse_check(source.is_relative_to((BASE/'candidates'/archive).resolve()),
                            'candidate source outside selected archive')
                _reuse_check(_sha256(anchor(source)) == item['sha256'], 'candidate changed')

    outer = read(outer_dir/'controller.json')
    inner = read(inner_dir/'controller.json')
    _reuse_check(outer.get('label') == label and outer.get('archive') == archive and
                outer.get('commit') == commit and outer.get('version') == version and
                outer.get('targets') == [ip+'='+ident for ip, (ident, _) in TARGETS.items()],
                'original outer candidate or targets differ')
    _reuse_check(outer.get('status') in ('failed', 'complete') and
                outer.get('completed') == (outer.get('status') == 'complete'), 'outer result missing')
    _reuse_check(inner.get('label') == label and inner.get('archive') == archive and
                _initial_gate(inner, commit, version), 'inner initial did not pass')
    stages = outer.get('stages', [])
    _reuse_check(bool(stages) and stages[0].get('name') == 'initial' and
                sum(row.get('name') == 'initial' for row in stages) == 1, 'original initial row missing')
    row = stages[0]
    _reuse_check(_process_complete(row, group=True) and row.get('pid') == inner['pid'] and
                Path(row.get('evidence_path', '')).resolve() == (inner_dir/'controller.json').resolve(),
                'original initial process evidence differs')
    argv = row.get('argv', [])
    _reuse_check(any(Path(str(arg)).name == 'run_initial_shared_screen.py' for arg in argv) and
                all(_contains_pair(argv, key, value) for key, value in
                    (('--archive', archive), ('--commit', commit), ('--version', version),
                     ('--label', label))) and '--execute' in argv, 'original initial argv differs')
    _reuse_check(_initial_previous_cli(inner, argv), 'initial previous override argv differs')
    previous = _initial_previous_versions(inner)
    selected = _target_scope(inner.get('selected_targets') and
                             {ip: TARGETS[ip] for ip in inner['selected_targets']} or None)
    legacy_full = 'selected_targets' not in inner and set(selected) == set(TARGETS)
    deploy_ips = tuple(ip for ip in selected if ip != tuple(selected)[0]) if inner.get('reused_deployments') else tuple(selected)
    for index, step in enumerate(inner['steps']):
        valid = _initial_step_argv(step, index, deploy_ips, label, archive, version,
                                   previous, selected, legacy_full)
        _reuse_check(valid, 'initial four-step argv/order mismatch')
    if inner.get('reused_deployments'):
        partial = inner['reused_deployments'][0]
        _reuse_check(_accepted_reuse_cli(partial, argv),
                     'partial source missing from actual initial argv')
        proofs.update(partial['proof_sha256'])
    outer_manifest_path = outer_dir/'source-manifest.json'
    outer_manifest = read(outer_manifest_path)
    _reuse_check(_sha256(outer_manifest_path) == outer.get('source_manifest_sha256') and
                outer_manifest.get('sources') == outer.get('sources') and
                outer_manifest.get('validation_commit') == outer.get('validation_commit'),
                'original outer source manifest changed')
    _reuse_check({'controller.py', 'initial_controller.py', 'readonly_stress.py',
                  'deploy_candidate.py', 'diagnose_shared_candidate.py', 'ota_package.py',
                  'audit_mixed_candidate_completion.py', 'http_status_schema_samples.py',
                  'http_mqtt_schema_parity.py'} <= set(outer_manifest['sources']),
                'original outer required sources missing')
    archived(outer_dir, outer_manifest['sources'])
    inner_manifest_path = inner_dir/'source-manifest.json'
    inner_manifest = read(inner_manifest_path)
    _reuse_check(_sha256(inner_manifest_path) == inner.get('source_manifest_sha256') and
                inner_manifest.get('candidate_commit') == commit and
                inner_manifest.get('candidate_version') == version and
                inner_manifest.get('commit') == inner.get('validation_commit'),
                'original inner source manifest changed')
    _reuse_check({'controller.py', 'readonly_stress.py', 'deploy_candidate.py',
                  'diagnose_shared_candidate.py', 'ota_package.py', 'status_benchmark.py',
                  'validate_shared_candidate.py'} <= set(inner_manifest.get('sources', {})),
                'original inner required sources missing')
    if inner.get('reused_deployments') and inner['reused_deployments'][0].get('schema') == ACCEPTED_UPLOAD.SCHEMA:
        for source_manifest in (outer_manifest, inner_manifest):
            _reuse_check('accepted_upload_postcheck.py' in source_manifest.get('sources', {}),
                         'accepted upload verifier source archive missing')
    archived(inner_dir, inner_manifest.get('sources'))
    files = outer.get('candidate_files', {})
    expected_files = set()
    for model in ('a1', 'tc1', 'm1'):
        ota = 'phicomm_'+('tc1_a1' if model == 'tc1' else model)+'.ota.bin'
        expected_files.update(model+'/'+name for name in ('manifest.json', 'SHA256SUMS', ota, ota+'.signed'))
    _reuse_check(set(files) == expected_files, 'incomplete original candidate archive')
    archived(outer_dir, files, current=True)
    from tools.ota.package import verify_package
    packages = {}
    for model in ('a1', 'tc1', 'm1'):
        folder = BASE/'candidates'/archive/model
        ota = 'phicomm_'+('tc1_a1' if model == 'tc1' else model)+'.ota.bin'
        manifest = read(folder/'manifest.json')
        raw = anchor(folder/ota).read_bytes()
        signed = anchor(folder/(ota+'.signed')).read_bytes()
        metadata = verify_package(signed, model)
        _reuse_check(manifest.get('git_commit') == commit and manifest.get('version') == version and
                    manifest.get('dirty') is False and
                    manifest.get('target') == {'a1': 'A1@MK3080B@moc', 'm1': 'M1@MK3080B@moc',
                                               'tc1': 'TC1@MK3031@moc'}[model] and
                    hashlib.sha256(raw).hexdigest() == manifest.get('files', {}).get(ota) and
                    metadata.get('model') == model and metadata.get('version') == manifest.get('base_version') and
                    metadata.get('commit') == commit and signed[192:] == raw,
                    'signed package or manifest identity mismatch')
        records = inner_manifest['sources']
        for name, data in ((model+'.ota.bin.signed', signed),
                           (model+'.manifest.json', (folder/'manifest.json').read_bytes())):
            _reuse_check(name in records and records[name]['sha256'] == hashlib.sha256(data).hexdigest(),
                        'inner deployed package differs')
        packages[model] = hashlib.sha256(signed).hexdigest()
    # Bind each actual deploy to its own expected receiver version and package.
    # A skipped A1 is verified through the separate immutable reused proof above.
    for index, ip in enumerate(deploy_ips):
        ident, model = TARGETS[ip]
        folder = PRIVATE/('deploy-'+label+'-initial-'+ip.rsplit('.', 1)[1])
        before, intent, upload, after = (read(folder/(name+'.json')) for name in
                                          ('before', 'upload-intent', 'upload', 'after'))
        manifest = read(BASE/'candidates'/archive/model/'manifest.json')
        _reuse_check(intent.get('ip') == ip and str(intent.get('device_id', '')).lower() == ident.lower() and
                     intent.get('sha256') == packages[model] and intent.get('manifest') == manifest and
                     upload.get('http') == 202 and upload.get('failed', False) is False,
                     'initial deploy package/target/202 mismatch')
        step = inner['steps'][index]
        times = (step.get('started_at'), intent.get('at'), step.get('ended_at'))
        _reuse_check(all(type(value) in (int, float) and math.isfinite(value) for value in times) and
                     times[0] <= times[1] <= times[2], 'initial upload intent outside child')
        for record, expected_version in ((before, previous[ip]), (after, version)):
            state = record.get('status', {})
            _reuse_check(str(state.get('device_id', '')).lower() == ident.lower() and
                         state.get('model') == model and state.get('version') == expected_version,
                         'initial deploy before/after identity/version mismatch')
        controls = {'a1': ('requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled'),
                    'tc1': ('socket_states', 'child_lock', 'led_enabled'), 'm1': ('brightness_requested',)}[model]
        _reuse_check(isinstance(before.get('config'), dict) and before['config'] == after.get('config') and
                     all(key in before['status'] and key in after['status'] and
                         before['status'][key] == after['status'][key] for key in controls),
                     'initial deploy config/controls changed')
        bu, au = before['status'].get('uptime_seconds'), after['status'].get('uptime_seconds')
        _reuse_check(all(type(value) in (int, float) and math.isfinite(value) for value in (bu, au)) and
                     0 <= au < bu, 'initial deploy reboot not observed')
    paced = read(paced_path)
    failures = []
    _runner_gate(paced, 'reused_paced', failures)
    _identity_gate(paced, version, failures, 'reused_paced')
    _reuse_check(not failures and paced.get('version') == version, 'original paced evidence failed')
    counts = {}
    for ip in TARGETS:
        for phase, expected in (('paced-full', 90), ('paced-live', 30)):
            rows = [item for item in paced.get('rows', [])
                    if item.get('ip') == ip and item.get('phase') == phase]
            _reuse_check(len(rows) == expected, 'original paced request count differs')
            counts[ip+' '+phase] = {
                'requests': len(rows),
                'http_200_ok': sum(item.get('http') == 200 and item.get('code') == 'ok' for item in rows),
                'http_503': sum(item.get('http') == 503 for item in rows),
                'no_http': sum('http' not in item for item in rows),
                'other_http': sum('http' in item and item.get('http') not in (200, 503) for item in rows),
            }
    _reuse_check(counts == inner.get('diagnostic_summary'), 'original paced summary differs')
    return {'schema': 'mixed-initial-reuse-v1', 'source_label': label,
            'archive': archive, 'commit': commit, 'version': version,
            'original_outer_status': outer['status'], 'original_outer_completed': outer['completed'],
            'initial_evidence_path': str((inner_dir/'controller.json').resolve()),
            'paced_evidence_path': str(paced_path.resolve()),
            'original_initial_stage': row, 'signed_package_sha256': packages,
            'proof_sha256': dict(sorted(proofs.items())),
            'interpretation': 'Only inner initial reused; original outer result is unchanged'}


def verify_reuse_continuity(reuse, pre, require_complete=True):
    old = _read_json(reuse['paced_evidence_path'])
    _reuse_check(_sha256(reuse['paced_evidence_path']) ==
                reuse['proof_sha256'][reuse['paced_evidence_path']], 'paced evidence changed')
    return verify_runner_continuity(old, pre, reuse['version'], require_complete)


def verify_runner_continuity(old, new, version, require_complete=True, targets=None):
    """Do not rebase away a reset/config/control change between runner instances.

    Status and memory counters have separate observations. Recorded A1/M1 clocks
    run faster than host monotonic time, so an intersection of all boot intervals
    would reject unchanged old evidence. Require nondecreasing uptime and no
    forward boot-origin shift beyond floor(seconds) uncertainty. This is sampled
    continuity, not a claim about every unobserved instant or clock failure.
    """
    selected = _target_scope(targets)
    failures = []
    _identity_gate(new, version, failures, 'continuity', selected)
    if require_complete:
        _runner_gate(new, 'continuity', failures, selected)
    else:
        _uptime_gate(new, 'continuity', failures, selected)
        _require(new.get('blockers') == [], 'continuity_blockers', failures)
    _reuse_check(not failures, 'new runner did not pass identity/uptime gates')

    def observations(evidence, ip, kind):
        base = evidence['before'][ip]
        result = ([(base['status_request_started_monotonic'],
                    base['status_request_ended_monotonic'], base['state']['uptime_seconds'], 1.0)]
                  if kind == 'status' else [])
        for row in evidence.get('rows' if kind == 'status' else 'memory', []):
            if row.get('ip') != ip or row.get('ok') is not True:
                continue
            if kind == 'status':
                if row.get('action') != 'status.get':
                    continue
                uptime = row.get('uptime')
            else:
                value = row.get('data', {}).get('uptime_ms')
                uptime = value/1000 if type(value) in (int, float) else None
            result.append((row.get('request_started_monotonic'),
                           row.get('request_ended_monotonic'), uptime,
                           1.0 if kind == 'status' else .001))
        _reuse_check(bool(result), 'missing continuity '+kind+' observations: '+ip)
        _reuse_check(all(all(type(v) in (int, float) and math.isfinite(v) for v in sample) and
                        sample[0] <= sample[1] and sample[2] >= 0 for sample in result),
                    'invalid continuity timing: '+ip)
        return sorted(result, key=lambda sample: sample[1])

    details = {}
    for ip, (_, model) in selected.items():
        left, right = old['before'][ip], new['before'][ip]
        _reuse_check(left['config'] == right['config'], 'configuration changed: '+ip)
        keys = {'a1': ('requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled'),
                'tc1': ('socket_states', 'child_lock', 'led_enabled'),
                'm1': ('brightness_requested',)}[model]
        _reuse_check(all(left['state'][key] == right['state'][key] for key in keys),
                    'controls changed: '+ip)
        streams = {}
        for kind in ('status', 'memory'):
            previous = observations(old, ip, kind)[-1]
            samples = observations(new, ip, kind)
            _reuse_check(all(start >= previous[1] for start, end, uptime, quantum in samples),
                        'new stage predates previous stage: '+ip)
            _reuse_check(all(uptime >= previous[2] and
                            start-uptime-quantum <= previous[1]-previous[2]
                            for start, end, uptime, quantum in samples), 'boot changed/reset: '+ip)
            streams[kind] = {'previous_end': previous[1], 'previous_uptime': previous[2],
                             'new_first_start': min(sample[0] for sample in samples)}
        details[ip] = {'streams': streams, 'configuration_equal': True, 'controls_equal': True}
    return {'passed': True, 'selected_targets': list(selected), 'targets': details,
            'limit': 'Sampled continuity; not a complete reboot history or proof against clock faults'}


def verify_reused_pre(reuse, checkpoint_path, journal_path, pre, label, reuse_hash):
    """Revalidate the pre-OTA checkpoint and the one authorized OTA-out per DUT."""
    checkpoint = _read_json(checkpoint_path)
    _reuse_check(checkpoint.get('schema') == 'reuse-pre-checkpoint-v1' and
                checkpoint.get('reuse_record_sha256') == reuse_hash,
                'pre-OTA checkpoint missing/wrong reuse record')
    continuity = verify_reuse_continuity(reuse, checkpoint['runner'], require_complete=False)
    _reuse_check(checkpoint.get('continuity') == continuity, 'pre-OTA checkpoint continuity differs')
    at = checkpoint.get('captured_monotonic')
    _reuse_check(type(at) in (int, float) and math.isfinite(at) and
                all(at >= item['status_request_ended_monotonic']
                    for item in checkpoint['runner']['before'].values()), 'invalid checkpoint time')
    journal = _read_json(journal_path)
    stage = label+'-pre-ota-out'
    steps = journal.get('steps', [])
    _reuse_check(journal.get('schema') == 'reuse-ota-out-v1' and journal.get('stage') == stage and
                journal.get('completed') is True and len(steps) == 3, 'OTA-out journal incomplete')
    proofs = {str(Path(checkpoint_path).resolve()): _sha256(checkpoint_path),
              str(Path(journal_path).resolve()): _sha256(journal_path)}
    for (ip, (ident, model)), row in zip(TARGETS.items(), steps):
        args = row.get('argv', [])
        suffix = ip.rsplit('.', 1)[1]
        start, end = row.get('started_monotonic'), row.get('ended_monotonic')
        _reuse_check(row.get('ip') == ip and type(row.get('pid')) is int and row['pid'] > 0 and
                    row.get('exit_code') == 0 and len(args) == 6 and
                    Path(str(args[1])).name == 'deploy_candidate.py' and
                    args[2:] == [suffix, stage, reuse['version'], reuse['archive']] and
                    type(start) in (int, float) and type(end) in (int, float) and
                    math.isfinite(start) and math.isfinite(end) and at <= start <= end,
                    'OTA-out happened before checkpoint or process/target mismatch')
        at = end
        folder = PRIVATE/('deploy-'+stage+'-'+suffix)
        evidence = {}
        for name in ('before', 'upload-intent', 'upload', 'after'):
            path = folder/(name+'.json')
            evidence[name] = _read_json(path)
            proofs[str(path.resolve())] = _sha256(path)
        intent = evidence['upload-intent']
        manifest = _read_json(BASE/'candidates'/reuse['archive']/model/'manifest.json')
        _reuse_check(intent.get('ip') == ip and str(intent.get('device_id', '')).lower() == ident.lower() and
                    intent.get('sha256') == reuse['signed_package_sha256'][model] and
                    intent.get('manifest') == manifest and evidence['upload'].get('http') == 202,
                    'OTA-out package/target/202 mismatch')
        before, after = evidence['before'], evidence['after']
        base = checkpoint['runner']['before'][ip]
        controls = {'a1': ('requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled'),
                    'tc1': ('socket_states', 'child_lock', 'led_enabled'),
                    'm1': ('brightness_requested',)}[model]
        for item in (before, after):
            state = item.get('status', {})
            _reuse_check(str(state.get('device_id', '')).lower() == ident.lower() and
                        state.get('model') == model and state.get('version') == reuse['version'] and
                        item.get('config') == base['config'] and
                        all(state.get(key) == base['state'][key] for key in controls),
                        'OTA-out identity/config/controls changed')
        bu, au = before['status'].get('uptime_seconds'), after['status'].get('uptime_seconds')
        _reuse_check(type(bu) in (int, float) and type(au) in (int, float) and
                    math.isfinite(bu) and math.isfinite(au) and 0 <= au < bu,
                    'OTA-out planned reboot not observed')
        latest = pre.get('before', {}).get(ip, {})
        state = latest.get('state', {})
        _reuse_check(latest.get('config') == base['config'] and
                    all(state.get(key) == base['state'][key] for key in controls),
                    'post-OTA config/controls changed')
        # The uploader records no exact status request interval. Its entire child
        # operation bounds the authorized reboot; do not invent a precise origin.
        ps, pe, pu = (latest.get('status_request_started_monotonic'),
                      latest.get('status_request_ended_monotonic'), state.get('uptime_seconds'))
        _reuse_check(all(type(v) in (int, float) and math.isfinite(v) for v in (ps, pe, pu)) and
                    ps >= end and pu >= au and ps-pu-1 <= end and pe-pu >= start,
                    'post-OTA boot outside authorized upload window')
    failures = []
    _identity_gate(pre, reuse['version'], failures, 'reuse_post_ota')
    _runner_gate(pre, 'reuse_post_ota', failures)
    _reuse_check(not failures, 'post-OTA pre gate failed')
    return {'passed': True, 'continuity': continuity, 'ota_out_count': 3,
            'proof_sha256': dict(sorted(proofs.items())),
            'boot_observation_limit': 'Post-OTA boot is bounded by each uploader child interval, not an exact reset timestamp'}


def verify_post_ota_chain(pre, stages, version, post=None, post_runner=None, targets=None):
    """All later initialized baselines share the post-OTA idle boot/config."""
    previous = pre
    report = {}
    for name, runner in stages:
        report[name] = verify_runner_continuity(previous, runner, version, targets=targets)
        previous = runner
    if post is not None:
        # The existing postcheck deliberately restores the soak's old before.
        # Its separate fresh comparison retains the real initialization times.
        fresh = copy.deepcopy(post_runner)
        for ip in _target_scope(targets):
            item = post.get('fresh_baseline_comparison', {}).get(ip, {})
            _reuse_check(all(item.get(key) is True for key in
                            ('identity_unchanged', 'model_version_unchanged', 'controls_unchanged',
                             'config_unchanged', 'uptime_continuity')), 'postcheck fresh comparison missing')
            start, end = item['fresh_status_interval']
            base = fresh['before'][ip]
            base['status_request_started_monotonic'] = start
            base['status_request_ended_monotonic'] = end
            base['state']['uptime_seconds'] = item['fresh_uptime_seconds']
        report['mixed_postcheck'] = verify_runner_continuity(previous, fresh, version, targets=targets)
    return {'passed': True, 'selected_targets': list(_target_scope(targets)), 'stages': report}


def verify_pre_soak_continuity(stage_map, version, targets=None):
    """Offline gate before launching 4h; does not issue any device request."""
    def evidence(name):
        row = stage_map[name]
        return Path(row.get('evidence_path', row.get('evidence', '')))
    pre_path = evidence('pre_stability')
    body_path = evidence('http_schema_samples').parent/'evidence.json'
    http_path = evidence('http_regression')
    short_controller_path = evidence('mixed_short')
    short_controller = _read_json(short_controller_path)
    short_path = Path(short_controller['output'])/'http-runner/evidence.json'
    proof = {str(path.resolve()): _sha256(path) for path in
             (pre_path, body_path, http_path, short_controller_path, short_path)}
    result = verify_post_ota_chain(_read_json(pre_path), [
        ('http_schema_samples', _read_json(body_path)),
        ('http_regression', _read_json(http_path)),
        ('mixed_short', _read_json(short_path))], version, targets=targets)
    return {'passed': True, 'continuity': result, 'proof_sha256': dict(sorted(proof.items()))}


def _audit_stage_argv(stage_map, label, archive, commit, version, failures, initial_label=None,
                      targets=None, allow_legacy_full=False):
    selected = _target_scope(targets)
    contracts = {
        'initial': ('run_initial_shared_screen.py',
                    (('--archive', archive), ('--commit', commit), ('--version', version),
                     ('--label', initial_label or label)), ('--execute',)),
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
    target_specs = [ip+'='+ident for ip, (ident, _model) in selected.items()]
    for name, (executable, pairs, flags) in contracts.items():
        row = stage_map.get(name, {})
        argv = row.get('argv') if isinstance(row.get('argv'), list) else []
        ok = any(executable in str(arg) for arg in argv)
        ok &= all(_contains_pair(argv, key, value) for key, value in pairs)
        ok &= all(flag in argv for flag in flags)
        if name in ('initial', 'pre_stability', 'mqtt_readonly', 'http_schema_samples', 'schema_parity',
                    'http_regression', 'mixed_short', 'mixed_soak', 'mixed_postcheck'):
            found = [argv[index+1] for index, item in enumerate(argv[:-1]) if item == '--target']
            ok &= found == ([] if allow_legacy_full else target_specs)
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


def audit_controller(controller_path, label, archive, commit, version, targets=None):
    controller_path = Path(controller_path)
    controller_dir = controller_path.parent
    controller = _read_json(controller_path)
    failures = []
    try:
        selected = _target_scope(targets if targets is not None else
                                 ({ip: TARGETS[ip] for ip in controller['selected_targets']}
                                  if 'selected_targets' in controller else None))
    except (ValueError, KeyError, TypeError):
        selected = {}
        failures.append('controller_target_scope_invalid')
    target_specs = [ip+'='+ident for ip, (ident, _model) in selected.items()]
    if 'selected_targets' in controller or 'device_status' in controller:
        target_record_ok = (controller.get('targets') == target_specs and
                            controller.get('selected_targets') == list(selected) and
                            controller.get('device_status') == {
                                ip: ('SELECTED' if ip in selected else 'PAUSED') for ip in TARGETS})
    else:
        target_record_ok = (set(selected) == set(TARGETS) and
                            controller.get('targets') == target_specs)
    _require(bool(selected) and target_record_ok,
             'controller_target_scope_or_paused_status', failures)
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
    reuse = None
    reuse_metadata = controller.get('initial_reuse')
    if reuse_metadata is not None:
        try:
            reuse_path = controller_dir/'initial-reuse.json'
            _reuse_check(reuse_metadata.get('path') == 'initial-reuse.json' and
                        _sha256(reuse_path) == reuse_metadata.get('sha256'), 'reuse record changed')
            recorded_reuse = _read_json(reuse_path)
            reuse = verify_reusable_initial(recorded_reuse['source_label'], archive, commit, version)
            _reuse_check(recorded_reuse == reuse, 'reuse proof differs from original evidence')
            expected_stage = dict(reuse['original_initial_stage'], execution='reused',
                                  reused_from=reuse['source_label'])
            _reuse_check(stage_map.get('initial') == expected_stage, 'reused stage provenance differs')
            _reuse_check(reuse['source_label'] != label, 'cannot reuse own label')
        except (OSError, ValueError, KeyError, TypeError) as exc:
            failures.append('initial_reuse_invalid:'+str(exc))
            reuse = None
    elif stage_map.get('initial', {}).get('execution') == 'reused':
        failures.append('initial_reuse_record_missing')
    _audit_stage_argv(stage_map, label, archive, commit, version, failures,
                      initial_label=reuse['source_label'] if reuse else None,
                      targets=selected,
                      allow_legacy_full=('selected_targets' not in controller and
                                         'device_status' not in controller))
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
    paths = controller.get('validation_paths')
    _require(isinstance(paths, list) and all(isinstance(path, str) and
             (path.startswith(('tests/', 'docs/')) or path == 'common/mqtt/README.md')
             for path in paths), 'candidate_validation_path_not_allowlisted', failures)

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

    initial_ok = bool(selected) and _initial_gate(initial, commit, version, selected)
    _require(initial_ok, 'initial_signed_ota_or_paced_gate', failures)
    expected_initial = PRIVATE/((reuse['source_label'] if reuse else label)+'-initial-controller')/'controller.json'
    _require(initial_path.resolve() == expected_initial.resolve(), 'initial_evidence_path_mismatch', failures)
    reuse_continuity = None
    if reuse is not None:
        try:
            checkpoint_path = PRIVATE/(label+'-pre-benchmark')/'reuse-continuity.json'
            journal_path = PRIVATE/(label+'-pre-benchmark')/'reuse-ota-out.json'
            reuse_continuity = verify_reused_pre(reuse, checkpoint_path, journal_path, pre, label,
                                                  reuse_metadata['sha256'])
            argv = stage_map.get('pre_stability', {}).get('argv', [])
            _reuse_check(_contains_pair(argv, '--reuse-initial-record',
                                       str(controller_dir/'initial-reuse.json')),
                        'pre lacks early continuity checkpoint argument')
            _reuse_check(controller.get('reuse_continuity') == reuse_continuity and
                        controller.get('reuse_pre_sha256') == _sha256(pre_path),
                        'continuity record or pre evidence changed')
        except (OSError, ValueError, KeyError, TypeError) as exc:
            failures.append('initial_reuse_continuity:'+str(exc))
    _require(pre.get('run', {}).get('status') == 'complete' and
             pre.get('run', {}).get('completed') is True and pre.get('blockers') == [],
             'pre_stability_only_runner_incomplete', failures)
    _identity_gate(pre, version, failures, 'pre_stability', selected)
    _runner_gate(pre, 'pre_stability', failures, selected)
    pre_checks = pre.get('completion_checks') if isinstance(pre.get('completion_checks'), dict) else {}
    _require(pre_checks.get('idle_quiescence') is True, 'pre_stability_idle_gate', failures)
    mqtt_readonly_summary = _audit_readonly_mqtt(mqtt, version, failures, selected)
    http_body_summary = _audit_http_body_evidence(http_body, version, failures,
                                                  http_body_runner, selected)
    _runner_gate(http_body_runner, 'http_schema_samples', failures, selected)
    _identity_gate(http_body_runner, version, failures, 'http_schema_samples', selected)
    schema_parity_summary = _audit_schema_parity(schema, version, failures, selected)
    _audit_http_regression(http, version, failures, selected)
    _require(short_controller.get('status') == 'complete' and short_controller.get('completed') is True and
             short_controller.get('source_unchanged') is True and
             short_controller.get('source_copies_unchanged') is True and
             short_controller.get('process', {}).get('exit_code') == 0 and
             short_controller.get('process', {}).get('timed_out') is False and
             short_controller.get('process', {}).get('terminated_after_failure_evidence') is False,
             'mixed_short_controller_incomplete', failures)
    short_runner_path = Path(short_controller.get('output', ''))/'http-runner/evidence.json'
    short_runner = _audit_mixed(short, short_runner_path, MIXED_SHORT_PLAN, version,
                                label+'-mixed-short', False, failures, 'mixed_short', selected)
    if short_runner is not None:
        _quiet_resources(short_runner, version, failures, 'mixed_short', require_queue=False,
                         targets=selected)
        short_load_summary = _audit_load_diagnostics(short_runner, failures, 'mixed_short', selected)
    else:
        short_load_summary = {}
    soak_runner_path = soak_path.parent/'http-runner/evidence.json'
    soak_runner = _audit_mixed(soak, soak_runner_path, MIXED_SOAK_PLAN, version,
                               label+'-mixed-soak', True, failures, 'mixed_soak', selected)
    if soak_runner is not None:
        quiet_summary = _quiet_resources(soak_runner, version, failures, 'mixed_soak', require_queue=True,
                                         targets=selected)
        soak_load_summary = _audit_load_diagnostics(soak_runner, failures, 'mixed_soak', selected)
    else:
        quiet_summary = {}
        soak_load_summary = {}
    _require(post.get('status') == 'complete' and post.get('completed') is True and
             post.get('source_unchanged') is True and post.get('source_copies_unchanged') is True and
             post.get('source_inputs_unchanged') is True and post.get('runner_completed') is True and
             post.get('exit_code') == 0 and post.get('process_group_empty') is True and
             post.get('process_group_cleanup_confirmed') is True,
             'mixed_postcheck_incomplete', failures)
    try:
        pre_soak = controller.get('pre_soak_continuity', {})
        expected_pre_soak = verify_pre_soak_continuity(stage_map, version, targets=selected)
        _require(all(pre_soak.get(key) == value for key, value in expected_pre_soak.items()),
                 'pre_soak_continuity_proof', failures)
        checked_at = pre_soak.get('checked_at')
        soak_started = stage_map.get('mixed_soak', {}).get('started_at')
        _require(type(checked_at) in (int, float) and type(soak_started) in (int, float) and
                 math.isfinite(checked_at) and math.isfinite(soak_started) and
                 checked_at <= soak_started, 'pre_soak_gate_after_start', failures)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        failures.append('pre_soak_continuity:'+str(exc))
    stage_continuity = None
    try:
        post_runner_path = post_path.parent/'runner/evidence.json'
        post_runner = _read_json(post_runner_path)
        stage_continuity = verify_post_ota_chain(pre, [
            ('http_schema_samples', http_body_runner), ('http_regression', http),
            ('mixed_short', short_runner), ('mixed_soak', soak_runner)], version, post, post_runner,
            targets=selected)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        failures.append('cross_stage_continuity:'+str(exc))
    output = {
        'schema': 1, 'label': label, 'archive': archive, 'commit': commit, 'version': version,
        'completion_audit_pass': not failures, 'failures': failures,
        'stages': {name: stage_map.get(name, {}).get('status', 'missing') for name in expected_names},
        'source_audit': source_audit, 'candidate_package_audit': candidate_audit,
        'initial_signed_ota_complete': initial_ok,
        'initial_reuse_verified': reuse is not None if reuse_metadata is not None else None,
        'initial_reuse_continuity': reuse_continuity,
        'post_ota_stage_continuity': stage_continuity,
        'pre_stability_only_complete': pre.get('run', {}).get('completed') is True,
        'mqtt_readonly_complete': mqtt_readonly_summary['passed'],
        'mqtt_readonly_summary': mqtt_readonly_summary,
        'http_schema_samples_complete': http_body_summary['passed'] and
                                        http_body_runner.get('run', {}).get('completed') is True,
        'http_schema_samples_summary': http_body_summary,
        'schema_parity_candidate_integrity': schema_parity_summary,
        'http_regression_complete': not any(item.startswith('http_regression_') for item in failures),
        'mixed_short_complete': short.get('completed') is True,
        'selected_targets': list(selected),
        'device_status': {ip: ('SELECTED' if ip in selected else 'PAUSED') for ip in TARGETS},
        'http_load_diagnostics': _audit_load_diagnostics(http, [], 'http_regression_report', selected),
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
    parser.add_argument('--target', action='append', metavar='IP=ID')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    report = audit_controller(args.controller, args.label, args.archive, args.commit,
                              args.version, targets=args.target)
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
