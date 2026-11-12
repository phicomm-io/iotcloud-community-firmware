#!/usr/bin/env python3
"""GCC14 code generation with the pinned MiCO GCC5 headers/linker/runtime."""
import json
import os
from pathlib import Path
import sys


def invocation(root, driver, arguments):
    args = list(arguments)
    query = any(x in args for x in ('--version', '-dumpversion', '-dumpfullversion'))
    compile_only = any(x in args for x in ('-c', '-E', '-S'))
    if '-o' in args and args.index('-o') + 1 < len(args):
        compile_only |= (args[args.index('-o') + 1].endswith('.o') and
                         any(x.lower().endswith(('.c', '.cc', '.cpp', '.cxx', '.s', '.asm')) for x in args))
    mode = 'query' if query else 'compile' if compile_only else 'link'
    actual = root / ('gcc14' if compile_only or query else 'gcc5') / 'bin' / driver
    if compile_only and not query:
        includes = [root / 'gcc14/lib/gcc/arm-none-eabi/14.3.1/include',
                    root / 'gcc5/lib/gcc/arm-none-eabi/5.4.1/include-fixed',
                    root / 'gcc5/arm-none-eabi/include']
        if driver.endswith('g++'):
            includes = [root / 'gcc5/arm-none-eabi/include/c++/5.4.1',
                        root / 'gcc5/arm-none-eabi/include/c++/5.4.1/arm-none-eabi',
                        root / 'gcc5/arm-none-eabi/include/c++/5.4.1/backward'] + includes
        prefix = ['--sysroot=' + str(root / 'gcc5/arm-none-eabi'), '-nostdinc']
        if driver.endswith('g++'):
            prefix.append('-nostdinc++')
        for include in includes:
            prefix += ['-isystem', str(include)]
        args = prefix + args
    return actual, args, mode


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[4]
    driver = Path(sys.argv[0]).name
    actual, args, mode = invocation(root, driver, sys.argv[1:])
    if os.environ.get('PHICOMM_COMPILER_LOG'):
        with open(os.environ['PHICOMM_COMPILER_LOG'], 'a') as log:
            log.write(json.dumps(dict(driver=driver, actual=str(actual), mode=mode, argv=args)) + '\n')
    os.execv(str(actual), [str(actual), *args])
