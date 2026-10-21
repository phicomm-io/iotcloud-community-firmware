#!/usr/bin/env python3
"""Require the actual checked numeric component in the final firmware ELF."""
import subprocess
import sys

REQUIRED = ('json_number_scan', 'json_number_format', 'json_num_scan_token',
            'json_num_strtod_r', 'json_num_dtoa_r', 'json_num_calloc', 'json_num_cleanup')


def check(symbols):
    names = [line.split()[-1] for line in symbols.splitlines() if line.split()]
    missing = [name for name in REQUIRED if names.count(name) != 1]
    if missing:
        raise ValueError('Checked JSON numeric component missing or duplicated: '+', '.join(missing))


if __name__ == '__main__':
    nm, image = sys.argv[1:]
    try:
        check(subprocess.check_output([nm, '--defined-only', image], text=True))
    except ValueError as error:
        sys.exit(str(error))
    print('Firmware checked JSON decimal parsing and formatting linked')
