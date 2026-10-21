#!/usr/bin/env python3
"""Copy the private numeric implementation after applying its SDK overlay.

The SDK remains independently disposable. Never copy into toolchain libc or
change SDK versions; every source is built as part of utilities/json_c.
"""
from pathlib import Path
import shutil
import sys


def install(sdk):
    root = Path(__file__).resolve().parents[1]
    destination = Path(sdk) / 'libraries/utilities/json_c'
    if 'json_numeric/context.c' not in (destination / 'json_c.mk').read_text():
        raise RuntimeError('apply json-number-checked.patch before copying sources')
    for name in ('json_number_checked.h', 'json_number_host.c'):
        shutil.copyfile(root/'common'/name, destination/name)
    shutil.copytree(root/'common/json_numeric', destination/'json_numeric', dirs_exist_ok=True)


if __name__ == '__main__':
    install(sys.argv[1])
