#!/usr/bin/env python3
"""Compare completed, matched read-only HTTP/MQTT evidence without secrets.

This reports measurements. Independent controller/source/package audits remain
required; this tool does not certify a release or infer leak freedom.
"""
import argparse
import json
import ipaddress
import re
import math
from pathlib import Path
import statistics

SUPPORTED_MODELS = ('A1', 'TC1', 'M1')
OUTCOMES = ('success', 'busy', 'timeout', 'transport', 'other', 'cancelled')
ZERO_FIELDS = ('memory_active', 'memory_reserved', 'memory_waiting', 'json_live_blocks')


def target_scope(specs=None, metadata=None):
    """Resolve an explicit scope or captured target metadata; never choose a DUT."""
    if specs is None:
        specs = metadata
    if not specs:
        raise ValueError('An explicit target scope or captured target metadata is required')
    if isinstance(specs, dict):
        entries = []
        for ip, value in specs.items():
            if isinstance(value, dict):
                model, identity = value.get('model'), value.get('device_id')
            elif isinstance(value, (tuple, list)) and len(value) == 2:
                model, identity = value
            else:
                raise ValueError('Target mapping must provide model and identity')
            entries.append((ip, model, identity))
    else:
        entries = []
        for spec in specs:
            if not isinstance(spec, str) or spec.count('=') != 1:
                raise ValueError('Targets must use IP=MODEL:ID')
            ip, value = spec.split('=', 1)
            if ':' not in value:
                raise ValueError('Targets must provide an explicit model: IP=MODEL:ID')
            model, identity = value.split(':', 1)
            entries.append((ip, model, identity))
    parsed, models, identities = {}, set(), set()
    for ip, model, identity in entries:
        try:
            if not isinstance(ip, str):
                raise ValueError
            address = str(ipaddress.ip_address(ip))
        except (TypeError, ValueError):
            raise ValueError('Target address is invalid') from None
        if not isinstance(model, str) or model.upper() not in SUPPORTED_MODELS:
            raise ValueError('Target model must be A1, TC1, or M1')
        model = model.upper()
        if not isinstance(identity, str) or not re.fullmatch(r'[0-9a-fA-F]{12}', identity):
            raise ValueError('Target identity must be 12 hexadecimal characters')
        identity = identity.lower()
        if address in parsed or model in models or identity in identities:
            raise ValueError('Target address, model, or identity is duplicated')
        parsed[address] = (model, identity)
        models.add(model)
        identities.add(identity)
    if 'M1' in models and models != set(SUPPORTED_MODELS):
        raise ValueError('M1 can only be selected with the full target set')
    return {ip: parsed[ip] for model in SUPPORTED_MODELS
            for ip in parsed if parsed[ip][0] == model}


def _is_ip(value):
    if not isinstance(value, str):
        return False
    try:
        ipaddress.ip_address(value)
        return True
    except (TypeError, ValueError):
        return False


def _scope_records_ok(document, selected):
    selected_ips = set(selected)
    valid = True

    def visit(value, parent_key=None):
        nonlocal valid
        if isinstance(value, dict):
            for key, child in value.items():
                if parent_key != 'device_status' and _is_ip(key) and key not in selected_ips:
                    valid = False
                if key == 'ip' and _is_ip(child) and child not in selected_ips:
                    valid = False
                if isinstance(key, str) and ' ' in key:
                    prefix = key.split(' ', 1)[0]
                    if _is_ip(prefix) and prefix not in selected_ips:
                        valid = False
                visit(child, key)
        elif isinstance(value, list):
            for child in value:
                visit(child, parent_key)

    visit(document)
    return valid


def _selection_status(run, selected):
    status = run.get('device_status')
    if not isinstance(status, dict) or any(
            not _is_ip(ip) or state != ('SELECTED' if ip in selected else 'PAUSED')
            for ip, state in status.items()) or not set(selected) <= set(status):
        raise ValueError('Target selection/paused status mismatch')
    return status


def number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def percentile(values, proportion):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * proportion) - 1)] if ordered else None


def summarize(rows, seconds):
    counts = {kind: sum(row.get('outcome') == kind for row in rows) for kind in OUTCOMES}
    if sum(counts.values()) != len(rows):
        raise ValueError('Unknown outcome')
    latencies = {}
    for kind in OUTCOMES:
        values = [row['elapsed_ms'] for row in rows
                  if row['outcome'] == kind and number(row.get('elapsed_ms'))]
        latencies[kind] = {
            'count': counts[kind], 'latency_samples': len(values),
            'p50_ms': statistics.median(values) if values else None,
            'p95_ms': percentile(values, .95), 'p99_ms': percentile(values, .99),
        }
    return {
        'attempts': len(rows), 'outcomes': counts,
        'success_per_second': counts['success'] / seconds,
        'success_percent': 100 * counts['success'] / len(rows) if rows else None,
        'busy_percent': 100 * counts['busy'] / len(rows) if rows else None,
        'latency_by_outcome': latencies,
    }


def load(path, targets=None):
    run = json.loads(path.read_text())
    if not isinstance(run, dict):
        raise ValueError('Mixed evidence must be an object')
    selected = target_scope(targets, metadata=run.get('targets'))
    if (run.get('completed') is not True or run.get('status') != 'complete' or
            run.get('blockers') != []):
        raise ValueError('Mixed run incomplete/blocked')
    if set(run.get('targets', {})) != set(selected):
        raise ValueError('Wrong target set')
    if not _scope_records_ok(run, selected):
        raise ValueError('Mixed evidence contains rows for an unselected device')
    if 'selected_targets' in run:
        if run.get('selected_targets') != list(selected):
            raise ValueError('Target selection/paused status mismatch')
        _selection_status(run, selected)
    elif {model for model, _identity in selected.values()} != set(SUPPORTED_MODELS):
        raise ValueError('Legacy evidence cannot be used for a subset comparison')
    runner = json.loads((path.parent / 'http-runner/evidence.json').read_text())
    if (runner.get('run', {}).get('completed') is not True or runner.get('blockers') != [] or
            any(runner.get('completion_checks', {}).get(key) is not True for key in
                ('network_restore', 'configuration_unchanged', 'idle_quiescence'))):
        raise ValueError('Underlying runner completion not proven')
    if set(runner.get('before', {})) != set(selected):
        raise ValueError('Underlying runner target set differs')
    if not _scope_records_ok(runner, selected):
        raise ValueError('Underlying runner contains rows for an unselected device')
    for ip, (model, identity) in selected.items():
        target = run['targets'][ip]
        baseline = runner.get('before', {}).get(ip, {})
        if (target.get('device_id', '').lower() != identity or
                target.get('model') != model.lower() or
                baseline.get('id', '').lower() != identity or
                baseline.get('state', {}).get('device_id', '').lower() != identity or
                baseline.get('state', {}).get('model') != model.lower() or
                baseline.get('state', {}).get('version') != run.get('version')):
            raise ValueError('Target identity/model/version mismatch')
    plan, phases = run.get('plan', []), run.get('phases', [])
    if (not plan or len({row['name'] for row in plan}) != len(plan) or
            [row.get('name') for row in phases] != [row['name'] for row in plan] or
            any(row.get('completed') is not True for row in phases)):
        raise ValueError('Incomplete phase plan')
    for expected, phase in zip(plan, phases):
        if (not number(phase.get('elapsed_seconds')) or
                phase['elapsed_seconds'] < expected['seconds'] or
                any(phase.get(key) != value for key, value in expected.items())):
            raise ValueError('Phase conditions/duration mismatch')
    return run, runner


def heap_median(runner, ip, phase):
    rows = [row.get('data', {}) for row in runner.get('memory', [])
            if row.get('ip') == ip and row.get('phase') == phase and row.get('ok') is True]
    # Require the actual last three samples to be quiescent, rather than
    # selecting earlier convenient zero samples from a still-active tail.
    tail = rows[-3:]
    if len(tail) != 3 or any(any(row.get(key) != 0 for key in ZERO_FIELDS) for row in tail):
        return None
    values = [row.get('free_memory') for row in tail]
    return statistics.median(values) if all(number(value) for value in values) else None


def telemetry(rows):
    def observed(row):
        return row.get('consumer_observed_monotonic', row.get('received_monotonic'))

    fresh = [row for row in rows if row.get('fresh') is True and
             row.get('validated') is True and row.get('retained') is False and
             number(observed(row))]
    times = sorted(observed(row) for row in fresh)
    gaps = [right - left for left, right in zip(times, times[1:])]
    uptimes = [row['uptime_source_seconds'] for row in fresh
               if number(row.get('uptime_source_seconds'))]
    return {
        'validated_live_arrivals': len(times),
        'gap_p95_seconds': percentile(gaps, .95),
        'gap_max_seconds': max(gaps) if gaps else None,
        'distinct_capture_uptimes': len(set(uptimes)),
        'source_uptime_span_seconds': max(uptimes) - min(uptimes) if uptimes else None,
        'arrival_span_seconds': times[-1] - times[0] if times else None,
    }


def phase_metrics(run, runner, ip, phase):
    name = phase['name']
    metrics = {kind: summarize([row for row in run[kind]
                               if row.get('ip') == ip and row.get('phase') == name],
                              phase['elapsed_seconds']) for kind in ('http', 'mqtt')}
    observations = [row for row in run.get('telemetry', [])
                    if row.get('ip') == ip and row.get('phase') == name]
    metrics['telemetry'] = telemetry(observations)
    separated = any('collection_stage' in row for row in observations)
    metrics['telemetry_collection_windows'] = {
        'separated_in_source': separated,
        'load_and_result_drain': telemetry([row for row in observations
                                          if row.get('collection_stage') == 'load']) if separated else None,
        'recovery': telemetry([row for row in observations
                               if row.get('collection_stage') == 'recovery']) if separated else None,
    }
    liveness = phase.get('mqtt_success_liveness', {}).get(ip, {})
    gaps = [liveness.get(key) for key in ('maximum_success_gap_seconds', 'last_success_gap_seconds')]
    metrics['mqtt_progress'] = {
        'maximum_success_gap_seconds': max(gaps) if all(number(value) for value in gaps) else None,
        'last_correlated_response_gap_seconds': liveness.get('last_correlated_response_gap_seconds'),
    }
    recovery = next((row for row in run.get('recovery', []) if row.get('phase') == name), {})
    mqtt = next((row for row in recovery.get('mqtt_results', []) if row.get('ip') == ip), {})
    http = next((row for row in runner.get('recoveries', []) if row.get('name') == name + '-http'), {})
    metrics['recovery'] = {
        'completed': recovery.get('completed') is True,
        'http_all_targets_elapsed_seconds': http.get('elapsed_seconds'),
        'http_target': http.get('targets', {}).get(ip),
        'mqtt_fresh_result_ms': mqtt.get('elapsed_ms'),
    }
    return metrics



def baseline_conditions(before, after, model):
    """Compare requested controls/configuration without exposing their values.

    Applied fan speed and sensor state can vary normally and are excluded.
    Missing legacy evidence makes attribution unknown rather than matched.
    """
    required = {
        'A1': ('requested_fan_on', 'requested_speed', 'child_lock', 'led_enabled'),
        'TC1': ('socket_states', 'child_lock', 'led_enabled'),
        'M1': ('brightness_requested',),
    }[model]
    left, right = before.get('state', {}), after.get('state', {})
    keys = list(required)
    if model == 'A1':
        keys += [key for key in ('fan_mode', 'auto_max_speed') if key in left or key in right]
    missing = [key for key in keys if key not in left or key not in right]
    def same(a, b):
        return json.dumps(a, sort_keys=True, allow_nan=False) == json.dumps(b, sort_keys=True, allow_nan=False)
    changed = [key for key in keys if key in left and key in right and not same(left[key], right[key])]
    config_available = isinstance(before.get('config'), dict) and isinstance(after.get('config'), dict)
    config_equal = same(before['config'], after['config']) if config_available else None
    matched = not missing and not changed and config_equal is True
    return {'matched': matched, 'missing_control_fields': missing,
            'changed_control_fields': changed, 'configuration_equal': config_equal,
            'attribution': 'matched_requested_controls_and_configuration' if matched else
                           'conditions_differ_or_are_unverified'}


def compare(before_path, after_path, targets=None):
    before, before_runner = load(before_path, targets)
    selected = target_scope(targets, metadata=before.get('targets'))
    after, after_runner = load(after_path, selected)
    if before.get('device_status') != after.get('device_status'):
        raise ValueError('Different target selection/paused status')
    if before['plan'] != after['plan'] or before['profile'] != after['profile']:
        raise ValueError('Different conditions')
    result = {
        'before_version': before['version'], 'after_version': after['version'],
        'profile': before['profile'], 'plan': before['plan'], 'models': {},
        'selected_targets': list(selected),
        'device_status': before.get('device_status', dict.fromkeys(selected, 'SELECTED')),
        'limits': [
            'Cross-run attribution requires baseline_conditions.matched; missing legacy controls/configuration are unverified.',
            'Bounded concurrent clients/windows; this is not a fixed offered-rate benchmark.',
            'Each outcome counts all attempts; latency samples are reported separately.',
            'Busy is response progress; gaps in successful results remain visible.',
            'Telemetry arrivals and capture uptime do not prove sensor sample age.',
            'Telemetry grouped by phase includes load and recovery collection; old evidence does not separate these windows.',
            'Telemetry timestamps record application queue consumption, which can drain buffered arrivals in a burst.',
            'HTTP recovery duration covers sequential verification of the explicitly selected targets.',
            'Finite heap medians do not prove leak freedom; unavailable metrics are not zero.',
            'Independent source/controller/package audits and closed SDK limits remain separate.',
        ],
    }
    for ip, (model, _identity) in selected.items():
        row = {'phases': {}, 'heap': {}, 'baseline_conditions': baseline_conditions(
            before_runner['before'][ip], after_runner['before'][ip], model)}
        for plan in before['plan']:
            pair = {}
            for label, run, runner in (('before', before, before_runner), ('after', after, after_runner)):
                phase = next(item for item in run['phases'] if item['name'] == plan['name'])
                pair[label] = phase_metrics(run, runner, ip, phase)
            row['phases'][plan['name']] = pair
        for label, runner in (('before', before_runner), ('after', after_runner)):
            first, last = (heap_median(runner, ip, phase) for phase in ('baseline', 'final'))
            row['heap'][label] = {
                'baseline_steady_bytes': first, 'final_steady_bytes': last,
                'delta_bytes': last - first if first is not None and last is not None else None,
            }
        result['models'][model] = row
    result['all_baseline_conditions_matched'] = all(
        row['baseline_conditions']['matched'] for row in result['models'].values())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--target', action='append', metavar='IP=MODEL:ID',
                        help='explicit expected scope; otherwise derive it from before capture metadata')
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError('Refusing to overwrite an existing comparison')
    report = compare(args.before, args.after, targets=args.target)
    with args.output.open('x', encoding='utf-8') as stream:
        args.output.chmod(0o600)
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write('\n')


if __name__ == '__main__':
    main()
