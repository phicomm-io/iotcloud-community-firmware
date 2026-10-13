#!/usr/bin/env python3
"""Check project versions without changing sources or SDK versions."""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    'TC1/main.h': 'VERSION',
    'TC1/mico_config.h': 'FIRMWARE_REVISION',
    'M1/m1.h': 'M1_VERSION',
    'M1/mico_config.h': 'FIRMWARE_REVISION',
    'A1/a1.h': 'A1_VERSION',
    'A1/mico_config.h': 'FIRMWARE_REVISION',
}


def project_version():
    version = (ROOT / '.version').read_text().strip()
    if not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', version):
        raise ValueError('Invalid project .version')
    for name, macro in SOURCES.items():
        match = re.search(r'^#define\s+' + macro + r'\s+"([^"]+)"',
                          (ROOT / name).read_text(), re.MULTILINE)
        if not match or match.group(1) != version:
            raise ValueError(f'{name}: {macro} must match .version ({version})')
    return version


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--tag')
    args = parser.parse_args()
    version = project_version()
    if args.tag and args.tag != 'v' + version:
        parser.error(f'Release tag must be v{version}, received {args.tag}')
    print(f'All three products use project version {version}')
