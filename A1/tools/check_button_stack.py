#!/usr/bin/env python3
"""Guard known synchronous button paths using the pinned compiler's .su files.

The reserve covers library/RTOS calls; this is not a runtime high-water mark.
"""
import re
import sys
from pathlib import Path


def check(directory, header):
    frames = {}
    for path in Path(directory).glob('*.su'):
        for line in path.read_text().splitlines():
            location, size, kind = line.split('\t')
            name = location.rsplit(':', 1)[-1].split('.')[0]
            if kind != 'static':
                continue
            frames[name] = max(frames.get(name, 0), int(size))
    budget = int(re.search(r'#define A1_HARDWARE_STACK_SIZE (\d+)u',
                           Path(header).read_text()).group(1))
    common = ['hardware_worker', 'automation_button', 'auto_execute',
              'service_command', 'service_product']
    paths = {
        'button status': common + ['service_query', 'a1_status_json'],
        'button control': common + ['service_product_inner', 'a1_control_local'],
    }
    for label, names in paths.items():
        missing = set(names) - frames.keys()
        if missing:
            raise ValueError('Missing stack usage: ' + ', '.join(sorted(missing)))
        application = sum(frames[name] for name in names)
        required = application + 2048
        if required > budget:
            raise ValueError(f'{label}: {application} application + 2048 reserve > {budget}')
        print(f'{label}: {application} application + 2048 reserve <= {budget} bytes')


if __name__ == '__main__':
    try:
        check(sys.argv[1], sys.argv[2])
    except (ValueError, IndexError, AttributeError, OSError) as error:
        sys.exit(str(error))
