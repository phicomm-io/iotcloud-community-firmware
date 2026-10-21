#!/usr/bin/env python3
"""Summarize read-only stress evidence without emitting configuration data."""
import argparse
import json
import math
from pathlib import Path
import sys


MEMORY_METRICS = (
    'free_memory', 'allocated_memory', 'free_memory_chunks', 'uptime_ms',
    'last_reset_reason', 'memory_reserve',
    'memory_reserved', 'memory_active', 'memory_waiting', 'memory_denied',
    'minimum_sampled_free', 'json_live_blocks', 'json_peak_blocks',
    'json_alloc_failures', 'json_guard_denied', 'http_tcp_limit',
    'http_tcp_connections', 'http_tcp_get_rc', 'mqtt_pending', 'mqtt_peak',
    'mqtt_overflow', 'mqtt_max_delay_ms',
    'http_core_owner_state', 'http_core_restarts', 'http_core_close_failures',
    'http_core_last_close_rc', 'http_core_last_close_errno',
    'http_core_slot_bytes', 'http_core_server_bytes',
    'http_core_last_select_rc', 'http_core_last_read_rc', 'http_core_occupied',
    'http_tcp_sample_ms', 'http_core_accepted', 'http_core_rejected',
    'http_core_tcp_limit', 'http_core_tcp_connections', 'http_core_tcp_rc',
    'http_core_read_errno',
)


def _number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    value = float(value)
    return value if math.isfinite(value) else None


def _percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def _classify_row(row):
    status = row.get('http')
    code = row.get('code')
    if isinstance(status, bool) or not isinstance(status, int):
        return 'no_http'
    if status == 503:
        return 'http_503'
    if status != 200:
        return 'unexpected'
    if code == 'busy':
        return 'busy'
    if row.get('ok') is True:
        return 'success'
    return 'unexpected'


def _memory_data(row):
    data = row.get('data')
    if not isinstance(data, dict):
        return {}
    return {key: value for key in MEMORY_METRICS
            if (value := _number(data.get(key))) is not None}


def _metric_slope(points):
    valid = [(at, value) for at, value in points
             if _number(at) is not None and _number(value) is not None]
    if len(valid) < 2:
        return None
    origin = valid[0][0]
    xs = [(at - origin) / 60.0 for at, _ in valid]
    ys = [value for _, value in valid]
    mean_x = sum(xs) / len(xs)
    mean_y = sum(ys) / len(ys)
    denom = sum((x - mean_x) ** 2 for x in xs)
    if denom == 0:
        return None
    return sum((x - mean_x) * (y - mean_y)
               for x, y in zip(xs, ys)) / denom


def _review_run(evidence):
    run = evidence.get('run')
    if not isinstance(run, dict):
        return {'validated_complete': False, 'reasons': ['missing_run_record']}
    if run.get('plan_completion_required') is not True:
        plan_required = False
    else:
        plan_required = True
    plan = run.get('plan') if isinstance(run.get('plan'), dict) else {}
    steps = plan.get('steps') if isinstance(plan.get('steps'), list) else []
    planned_phases = plan.get('phases') if isinstance(plan.get('phases'), list) else []
    recovery_steps = plan.get('recovery_steps') if isinstance(plan.get('recovery_steps'), list) else []
    scenarios = evidence.get('scenarios') if isinstance(evidence.get('scenarios'), list) else []
    phases = evidence.get('phases') if isinstance(evidence.get('phases'), list) else []
    recoveries = evidence.get('recoveries') if isinstance(evidence.get('recoveries'), list) else []
    checks = evidence.get('completion_checks')
    if not isinstance(checks, dict):
        checks = {}
    reasons = []
    if not plan_required:
        reasons.append('plan_completion_not_required')
    if not steps:
        reasons.append('missing_planned_steps')
    mode = plan.get('mode')
    if mode == 'full':
        required_steps = [
            'concurrency-1', 'concurrency-4', 'concurrency-8', 'concurrency-16',
            'slow-clients', 'aborted-clients', 'buffered-read-clients',
            'missing-read-clients', 'soak',
        ]
        required_phases = ['concurrency-1', 'concurrency-4', 'concurrency-8',
                           'concurrency-16', 'soak']
        if steps != required_steps:
            reasons.append('invalid_full_plan_steps')
    elif mode == 'soak-only':
        required_steps = ['soak']
        required_phases = ['soak']
        if steps != required_steps:
            reasons.append('invalid_soak_only_plan_steps')
    else:
        required_steps, required_phases = [], []
        reasons.append('invalid_plan_mode')
    if [phase.get('name') for phase in planned_phases if isinstance(phase, dict)] != required_phases:
        reasons.append('invalid_planned_phases')
    plan_targets = plan.get('targets')
    if not isinstance(plan_targets, list) or not plan_targets:
        plan_targets = []
        reasons.append('missing_plan_targets')
    target_ips = [target.get('ip') for target in plan_targets
                  if isinstance(target, dict) and isinstance(target.get('ip'), str)]
    if len(target_ips) != len(plan_targets) or len(set(target_ips)) != len(target_ips):
        reasons.append('invalid_plan_targets')
    expected_recoveries = [
        {'name': name+'-recovery', 'targets': target_ips}
        for name in required_phases]
    if mode == 'full':
        expected_recoveries.extend((
            {'name': name, 'targets': target_ips}
            for name in ('incomplete-headers-recovery',
                         'aborted-connections-recovery',
                         'buffered-read-input-recovery',
                         'missing-read-eof-recovery')
        ))
    recovery_plan_valid = (
        len(recovery_steps) == len(expected_recoveries) and
        all(isinstance(actual, dict) and
            actual.get('name') == expected.get('name') and
            actual.get('targets') == expected.get('targets')
            for actual, expected in zip(recovery_steps, expected_recoveries))
    )
    if not recovery_plan_valid:
        reasons.append('invalid_planned_recoveries')
    for step in steps:
        matches = [item for item in scenarios if isinstance(item, dict)
                   and item.get('name') == step]
        if len(matches) != 1 or matches[0].get('completed') is not True:
            reasons.append('scenario_incomplete:'+str(step))
    for planned in planned_phases:
        if not isinstance(planned, dict):
            reasons.append('invalid_phase_plan')
            continue
        name = planned.get('name')
        matches = [item for item in phases if isinstance(item, dict)
                   and item.get('name') == name]
        if len(matches) != 1:
            reasons.append('phase_missing_or_duplicate:'+str(name))
            continue
        elapsed = _number(matches[0].get('elapsed_seconds'))
        requested = _number(planned.get('requested_seconds'))
        actual_requested = _number(matches[0].get('requested_seconds'))
        if (matches[0].get('completed') is not True or elapsed is None or
                requested is None or elapsed < requested or
                actual_requested != requested or
                matches[0].get('clients_per_device') != planned.get('clients_per_device')):
            reasons.append('phase_incomplete_or_short:'+str(name))
    for planned in recovery_steps:
        if not isinstance(planned, dict):
            continue
        name = planned.get('name')
        expected_targets = set(planned.get('targets', []))
        matches = [item for item in recoveries if isinstance(item, dict)
                   and item.get('name') == name]
        covered = set()
        valid = bool(expected_targets) and expected_targets <= set(target_ips)
        for match in matches:
            if (match.get('completed') is not True or
                    match.get('required_consecutive_successes') != 3):
                valid = False
            target_results = match.get('targets')
            if not isinstance(target_results, dict):
                valid = False
                continue
            for ip, result in target_results.items():
                if ip not in expected_targets or ip in covered:
                    valid = False
                    continue
                covered.add(ip)
                if (not isinstance(result, dict) or
                        result.get('completed') is not True or
                        result.get('consecutive_successes') != 3):
                    valid = False
        if covered != expected_targets:
            valid = False
        if not valid:
            reasons.append('recovery_incomplete:'+str(name))
    for name in ('network_restore', 'configuration_unchanged', 'idle_quiescence'):
        if checks.get(name) is not True:
            reasons.append('completion_check_failed:'+name)
    blockers = evidence.get('blockers')
    if not isinstance(blockers, list):
        reasons.append('missing_blocker_list')
    elif blockers:
        reasons.append('evidence_has_blockers')
    recorded_complete = run.get('status') == 'complete' and run.get('completed') is True
    if not recorded_complete:
        reasons.append('run_not_recorded_complete')
    return {
        'run_id': run.get('run_id') if isinstance(run.get('run_id'), str) else None,
        'mode': mode if isinstance(mode, str) else None,
        'recorded_status': run.get('status') if isinstance(run.get('status'), str) else None,
        'recorded_complete': recorded_complete,
        'validated_complete': not reasons,
        'reasons': reasons,
        'planned_steps': steps,
        'scenarios': [
            {'name': row.get('name'), 'completed': row.get('completed') is True,
             'elapsed_seconds': _number(row.get('elapsed_seconds'))}
            for row in scenarios if isinstance(row, dict)
        ],
        'phases': [
            {'name': row.get('name'), 'clients_per_device': row.get('clients_per_device'),
             'requested_seconds': _number(row.get('requested_seconds')),
             'elapsed_seconds': _number(row.get('elapsed_seconds')),
             'completed': row.get('completed') is True}
            for row in phases if isinstance(row, dict)
        ],
        'recoveries': [_recovery_summary(row) for row in recoveries
                       if isinstance(row, dict)],
        'completion_checks': {
            name: checks.get(name) is True
            for name in ('network_restore', 'configuration_unchanged', 'idle_quiescence')
        },
    }


def _recovery_summary(row):
    targets = row.get('targets')
    if not isinstance(targets, dict):
        targets = {}
    return {
        'name': row.get('name'),
        'completed': row.get('completed') is True,
        'required_consecutive_successes': row.get('required_consecutive_successes'),
        'targets': {
            ip: {'attempts': value.get('attempts'),
                 'consecutive_successes': value.get('consecutive_successes'),
                 'completed': value.get('completed') is True}
            for ip, value in targets.items() if isinstance(value, dict)
        },
    }


def summarize_evidence(evidence, test_complete=False):
    """Return a compact, allowlisted view of a Runner evidence object."""
    if not isinstance(evidence, dict):
        raise ValueError('evidence root must be a JSON object')

    before = evidence.get('before')
    if not isinstance(before, dict):
        before = {}
    models = {}
    for ip, record in before.items():
        state = record.get('state') if isinstance(record, dict) else None
        model = state.get('model') if isinstance(state, dict) else None
        models[ip] = model if isinstance(model, str) else 'unknown'

    grouped = {}
    rows = evidence.get('rows')
    if not isinstance(rows, list):
        rows = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        ip = row.get('ip') if isinstance(row.get('ip'), str) else 'unknown'
        phase = row.get('phase') if isinstance(row.get('phase'), str) else 'unknown'
        model = models.get(ip, 'unknown')
        key = (model, ip, phase)
        group = grouped.setdefault(key, {
            'requests': 0, 'success': 0, 'degraded': 0,
            'http_503': 0, 'busy': 0, 'no_http': 0, 'unexpected': 0,
            'success_latencies_ms': [],
        })
        group['requests'] += 1
        category = _classify_row(row)
        if category == 'success':
            group['success'] += 1
            latency = _number(row.get('ms'))
            if latency is not None and latency >= 0:
                group['success_latencies_ms'].append(latency)
        else:
            group['degraded'] += 1
            group[category] += 1

    phase_summaries = []
    for (model, ip, phase), group in sorted(grouped.items()):
        latency = group.pop('success_latencies_ms')
        requests = group['requests']
        phase_summaries.append({
            'model': model,
            'ip': ip,
            'phase': phase,
            'requests': requests,
            'success': group['success'],
            'degraded': group['degraded'],
            'success_rate': group['success'] / requests if requests else 0.0,
            'http_503': group['http_503'],
            'busy': group['busy'],
            'no_http': group['no_http'],
            'unexpected': group['unexpected'],
            'success_latency_ms': {
                'sample_count': len(latency),
                'p50': _percentile(latency, .50),
                'p95': _percentile(latency, .95),
                'p99': _percentile(latency, .99),
            },
        })

    memory = evidence.get('memory')
    if not isinstance(memory, list):
        memory = []
    memory_history = []
    trends = {}
    for row in memory:
        if not isinstance(row, dict):
            continue
        ip = row.get('ip') if isinstance(row.get('ip'), str) else 'unknown'
        phase = row.get('phase') if isinstance(row.get('phase'), str) else 'unknown'
        model = models.get(ip, 'unknown')
        at = _number(row.get('at'))
        ok = row.get('ok') is True
        metrics = _memory_data(row) if ok else {}
        record = {
            'model': model,
            'ip': ip,
            'phase': phase,
            'at': at,
            'ok': ok,
            'http_status': row.get('http') if isinstance(row.get('http'), int) else None,
            'metrics': metrics,
        }
        memory_history.append(record)
        key = (model, ip, phase)
        series = trends.setdefault(key, {})
        for metric, value in metrics.items():
            series.setdefault(metric, []).append((at, value))

    memory_history.sort(key=lambda row: (
        row['ip'], row['phase'], row['at'] if row['at'] is not None else -1))
    trend_summaries = []
    for (model, ip, phase), series in sorted(trends.items()):
        metric_summaries = {}
        for metric, points in sorted(series.items()):
            points.sort(key=lambda point: (
                point[0] if point[0] is not None else math.inf))
            values = [value for _, value in points]
            metric_summaries[metric] = {
                'sample_count': len(points),
                'first': points[0][1],
                'last': points[-1][1],
                'minimum': min(values),
                'maximum': max(values),
                'delta': points[-1][1] - points[0][1],
                'slope_per_minute': _metric_slope(points),
            }
        trend_summaries.append({
            'model': model,
            'ip': ip,
            'phase': phase,
            'samples': max((len(v) for v in series.values()), default=0),
            'first_at': min((at for values in series.values() for at, _ in values
                             if at is not None), default=None),
            'last_at': max((at for values in series.values() for at, _ in values
                            if at is not None), default=None),
            'metrics': metric_summaries,
        })

    blockers = evidence.get('blockers')
    blocker_count = len(blockers) if isinstance(blockers, list) else 0
    return {
        'source_version': evidence.get('version') if isinstance(evidence.get('version'), str) else None,
        'test_completion': {
            'operator_declared_complete': bool(test_complete),
            'evidence_blocker_count': blocker_count,
            'assessment': 'descriptive evidence only; no leak-free or release-pass determination',
        },
        'run_review': _review_run(evidence),
        'model_phase_summary': phase_summaries,
        'memory_history': memory_history,
        'memory_trends': trend_summaries,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence', type=Path, help='Existing readonly_stress evidence.json')
    parser.add_argument('--test-complete', action='store_true',
                        help='Record an explicit caller declaration; does not imply PASS')
    args = parser.parse_args(argv)
    try:
        with args.evidence.open('r', encoding='utf-8') as stream:
            evidence = json.load(stream)
        summary = summarize_evidence(evidence, args.test_complete)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        parser.error(f'cannot summarize evidence: {type(exc).__name__}')
    json.dump(summary, sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write('\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
