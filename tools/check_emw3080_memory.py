#!/usr/bin/env python3
"""Check saved A1/M1 release memory samples against the 12 KiB stress target.

The runtime allocation floor is independently 8 KiB. This checks sampled heap
evidence only; it does not certify Wi-Fi SDK headroom between samples.
"""
import argparse
import json
import math
from pathlib import Path
import sys


RUNTIME_RESERVE_BYTES = 8192
PRESSURE_MINIMUM_BYTES = 12288
MINIMUM_FIELDS = (
    'minimum_sampled_free', 'min_observed_free_memory',
    'minimum_free', 'min_free_memory', 'minimum_free_memory',
)
METRICS = ('free_memory', 'uptime_ms', 'memory_reserve') + MINIMUM_FIELDS


def assess(samples):
    """Return allowlisted values and failures without discarding low samples."""
    records, failures, minima = [], [], []
    for index, sample in enumerate(samples):
        data = sample.get('data', sample) if isinstance(sample, dict) else None
        metrics, invalid_metrics = {}, {}
        if isinstance(data, dict):
            for key in METRICS:
                if key not in data:
                    continue
                value = data[key]
                if type(value) is int:
                    metrics[key] = value  # Retain negative diagnostic values.
                if type(value) is not int or value < 0:
                    diagnostic = {'type': type(value).__name__}
                    if type(value) in (int, bool) or (type(value) is float and math.isfinite(value)):
                        diagnostic['value'] = value
                    invalid_metrics[key] = diagnostic
        reasons = []
        reasons.extend('invalid_metric_'+key for key in invalid_metrics)
        if not isinstance(sample, dict) or sample.get('code', 'ok') != 'ok':
            reasons.append('invalid_sample')
        if metrics.get('memory_reserve') != RUNTIME_RESERVE_BYTES:
            reasons.append('release_reserve_must_be_8192')
        minimum = [metrics[key] for key in MINIMUM_FIELDS if key in metrics]
        if not minimum:
            reasons.append('missing_sampled_minimum')
        else:
            if 'free_memory' in metrics:
                minimum.append(metrics['free_memory'])
            minima.extend(minimum)
            if min(minimum) < PRESSURE_MINIMUM_BYTES:
                reasons.append('sampled_minimum_below_12288')
        record = {'sample': index, 'metrics': metrics, 'failures': reasons}
        if invalid_metrics:
            record['invalid_metrics'] = invalid_metrics
        records.append(record)
        failures.extend({'sample': index, 'reason': reason} for reason in reasons)
    if not records:
        failures.append({'reason': 'no_samples'})
    return {
        'passed': not failures,
        'runtime_reserve_bytes': RUNTIME_RESERVE_BYTES,
        'pressure_minimum_bytes': PRESSURE_MINIMUM_BYTES,
        'minimum_sampled_free': min(minima) if minima else None,
        'samples': records,
        'failures': failures,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshots', type=Path, nargs='+',
                        help='Saved /api/v1/memory JSON object or JSON list of samples')
    args = parser.parse_args(argv)
    samples = []
    try:
        for path in args.snapshots:
            value = json.loads(path.read_text(encoding='utf-8'))
            samples.extend(value if isinstance(value, list) else [value])
    except (OSError, json.JSONDecodeError) as exc:
        parser.error(f'cannot read memory samples: {type(exc).__name__}')
    result = assess(samples)
    json.dump(result, sys.stdout, indent=2)
    sys.stdout.write('\n')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
