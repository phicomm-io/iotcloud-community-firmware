#!/usr/bin/env python3
"""Stop forcing unused newlib-nano float printf into A1/M1 private SDKs.

Binary MessagePack numbers and the application decimal formatter are independent
of printf. SDK logging/provisioning and HA callers use only integer/string
conversions. Keep nano libc, normal printf, all SDK sources and compile flags.
TC1 already makes this selection in install_tc1_native_sdk.py.
"""
from pathlib import Path
import sys

OLD = b'GLOBAL_LDFLAGS   += $$(CLIB_LDFLAGS_NANO_FLOAT)'
NEW = b'GLOBAL_LDFLAGS   += $$(CLIB_LDFLAGS_NANO) # no application/SDK printf float caller'


def install(path):
    core = Path(path) / 'MiCO/core/core.mk'
    before = core.read_bytes()
    if before.count(OLD) == 1 and NEW not in before:
        core.write_bytes(before.replace(OLD, NEW))
    elif before.count(NEW) != 1 or OLD in before:
        raise RuntimeError('unsupported SDK nano printf link flags')


if __name__ == '__main__':
    install(sys.argv[1])
