#!/usr/bin/env python3
"""Reject JSON DOM/parser dependencies in final MiCO ELF files.

Daikin is intentionally outside this gate. A small HA writer/parser is allowed;
retaining the old SDK object library is not. Inspect the linked ELF, not archives.
"""
import argparse
from pathlib import Path
import re
import subprocess

FORBIDDEN = re.compile(r'^(?:_?json_object(?:_|$)|_?json_tokener(?:_|$)|_?json_num_(?:strtod|dtoa)|_?json_c_(?:object|version))')


def inspect(elf, nm):
    result = subprocess.run([nm, '--defined-only', str(elf)], check=True,
                            text=True, capture_output=True)
    return sorted({line.split()[-1] for line in result.stdout.splitlines()
                   if line.split() and FORBIDDEN.match(line.split()[-1])})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nm', default='arm-none-eabi-nm')
    parser.add_argument('--model', required=True, choices=['a1', 'm1', 'tc1'])
    parser.add_argument('elf', type=Path)
    args = parser.parse_args()
    if not args.elf.is_file():
        parser.error('final ELF does not exist')
    try:
        forbidden = inspect(args.elf, args.nm)
    except (OSError, subprocess.CalledProcessError) as error:
        parser.exit(2, f'Cannot inspect final ELF: {error}\n')
    if forbidden:
        parser.exit(1, 'JSON object dependency remains:\n' + '\n'.join(forbidden) + '\n')
    print(f'{args.model}: no legacy JSON object/parser symbols in {args.elf}')


if __name__ == '__main__':
    main()
