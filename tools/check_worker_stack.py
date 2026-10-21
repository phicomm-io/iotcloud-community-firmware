#!/usr/bin/env python3
"""Conservative application-frame budgets for synchronous task call paths.

Pinned GCC .su data plus reserves for SDK/JSON/RTOS; not runtime watermarks.
MQTT owner paths are checked separately with their compiled shared archive.
"""
import re
import sys
from pathlib import Path


def check(directory, header):
    directory = Path(directory)
    frames = {}
    for path in list(directory.glob('*.su')) + list((directory.parent/'phicomm_httpd').glob('*.su')):
        for line in path.read_text().splitlines():
            location, size, kind = line.split('\t')
            name = location.rsplit(':', 1)[-1].split('.')[0]
            if kind == 'static':
                frames[name] = max(frames.get(name, 0), int(size))
    binding = int(re.search(r'#define FEATURE_WORKER_STACK_SIZE (\d+)u',
                            Path(header).read_text()).group(1))
    paths = {
        'scheduled control': (binding, 2048, ['feature_worker', 'automation_poll',
            'auto_execute', 'service_command', 'service_product', 'service_product_inner']),
        'scheduled status': (binding, 2048, ['feature_worker', 'automation_poll',
            'auto_execute', 'service_query', 'service_command', 'service_product',
            'a1_status_json' if directory.name == 'A1' else 'm1_status_json',
            'a1_status_json_view' if directory.name == 'A1' else 'm1_status_json_view']),
    }
    status='a1_status_json' if directory.name=='A1' else 'm1_status_json'
    if directory.name=='A1':paths['scheduled control'][2].extend(['a1_control_local','feature_group_control','group_send','parse'])
    # MQTT paths are checked with the linked shared archive in check_http_core.
    expanded={}
    for label,(budget,reserve,names) in paths.items():
        if status not in names:
            expanded[label]=(budget,reserve,names);continue
        common=names+[directory.name.lower()+'_status_full']
        expanded[label+' capture']=(budget,reserve,common+[directory.name.lower()+'_status_capture']+[n for n in ('runtime_metrics_capture','feature_fan_snapshot','watchdog_status_capture','a1_filter_get','ph_http_snapshot','tc1_status_begin','tc1_ota_snapshot','reset_name') if n in frames])
        encode=['device_status_encode','encode_'+directory.name.lower()]
        encode += [n for n in ('device_runtime_encode','device_http_encode','device_filter_encode','watchdog_status_encode') if n in frames]
        expanded[label+' encode']=(budget,reserve,common+encode)
    paths=expanded
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
