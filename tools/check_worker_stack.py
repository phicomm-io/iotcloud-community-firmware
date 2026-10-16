#!/usr/bin/env python3
"""Conservative application-frame budgets for synchronous task call paths.

Pinned GCC .su data plus reserves for SDK/JSON/RTOS; not runtime watermarks.
MQTT reserves include packet dispatch while a QoS1 publish awaits its ACK.
"""
import re
import sys
from pathlib import Path


def check(directory, header):
    directory = Path(directory)
    frames = {}
    for path in directory.glob('*.su'):
        for line in path.read_text().splitlines():
            location, size, kind = line.split('\t')
            name = location.rsplit(':', 1)[-1].split('.')[0]
            if kind == 'static':
                frames[name] = max(frames.get(name, 0), int(size))
    binding = int(re.search(r'#define FEATURE_WORKER_STACK_SIZE (\d+)u',
                            Path(header).read_text()).group(1))
    source = Path(header).resolve().parents[1] / directory.name / 'mqtt.c'
    mqtt_budget = int(re.search(r'mqtt_worker, (\d+), 0', source.read_text()).group(1))
    paths = {
        'scheduled control': (binding, 2048, ['feature_worker', 'automation_poll',
            'auto_execute', 'service_command', 'service_product', 'service_product_inner']),
        'scheduled status': (binding, 2048, ['feature_worker', 'automation_poll',
            'auto_execute', 'service_query', 'service_command', 'service_product',
            'a1_status_json' if directory.name == 'A1' else 'm1_status_json']),
    }
    if directory.name == 'A1':
        paths['MQTT nested virtual command'] = (mqtt_budget, 4096, ['mqtt_worker',
            'feature_mqtt_service', 'discovery_virtual', 'feature_command',
            'feature_mqtt_input', 'service_command', 'service_product',
            'service_product_inner', 'a1_control_local'])
    else:
        paths['MQTT nested command'] = (mqtt_budget, 4096, ['mqtt_worker', 'discovery',
            'native_command', 'service_command', 'service_product', 'service_product_inner'])
    http_source = source.with_name('http.c').read_text()
    accept_budget = int(re.search(r'http_accept_worker,\s*(\d+)', http_source).group(1))
    paths['HTTP accept'] = (accept_budget, 1536, ['http_accept_worker'])
    if directory.name == 'A1':
        for label in ('scheduled control', 'MQTT nested virtual command'):
            paths[label][2].extend(['feature_group_control', 'group_send', 'parse'])
    for label, (budget, reserve, names) in paths.items():
        missing = set(names) - frames.keys()
        if missing:
            raise ValueError('Missing stack usage: ' + ', '.join(sorted(missing)))
        application = sum(frames[name] for name in names)
        if application + reserve > budget:
            raise ValueError(f'{label}: {application} application + {reserve} reserve > {budget}')
        print(f'{label}: {application} application + {reserve} reserve <= {budget} bytes')


if __name__ == '__main__':
    try:
        check(sys.argv[1], sys.argv[2])
    except (ValueError, IndexError, AttributeError, OSError) as error:
        sys.exit(str(error))
