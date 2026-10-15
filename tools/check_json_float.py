#!/usr/bin/env python3
"""Reject firmware that silently cannot parse decimal JSON with nano libc."""
import subprocess
import sys

nm, image = sys.argv[1:]
symbols = subprocess.check_output([nm, '--defined-only', image], text=True)
if not any(line.split()[-1:] == ['_scanf_float'] for line in symbols.splitlines()):
    raise SystemExit('JSON decimal parsing unavailable: link with -u _scanf_float')
print('Firmware JSON decimal input support linked')
