"""Measure startup/persistence frames with the native build's compiler flags.

These are per-function limits, not a bound on callees or interrupt context.
Keep the compiler report after disposable SDK/build directories are removed.
"""
import json
from pathlib import Path
import shlex
import subprocess


LIMITS = {'save_record': 256, 'PH_BL_Init': 2560, 'PH_BL_Tick': 256}


def check_report(text):
    frames = {}
    for line in text.splitlines():
        origin, amount, kind = line.rsplit('\t', 2)
        function = origin.rsplit(':', 1)[-1]
        for name in LIMITS:
            if function == name or function.startswith(name + '.'):
                frames[function] = {'bytes': int(amount), 'kind': kind}
    for name, limit in LIMITS.items():
        found = [frame for function, frame in frames.items()
                 if function == name or function.startswith(name + '.')]
        if not found:
            raise ValueError('Missing native stack report for ' + name)
        if any(frame['kind'] != 'static' or frame['bytes'] > limit for frame in found):
            raise ValueError(f'{name} exceeds its {limit}-byte static frame budget: {found}')
    return frames


def verify_stack(run, version, gcc):
    run = Path(run)
    output = run / 'stack-check'
    output.mkdir(exist_ok=True)
    cwd = run / 'build/sdk/platforms/bk7231n/bk7231n_os'
    command = ['make', '-f', 'application.mk', '-n', '-B',
               '../../../apps/OpenBK7231N/src/phicomm/src/ph_port.o',
               'APP_BIN_NAME=OpenBK7231N', f'USER_SW_VER={version}',
               f'APP_VERSION={version}', 'TARGET_PLATFORM=bk7231n',
               'OBK_VARIANT=0', 'CFG_USE_MQTT_TLS=0',
               f'ARM_GCC_TOOLCHAIN={Path(gcc).parent}']
    planned = subprocess.check_output(command, cwd=cwd, text=True)
    (output / 'make.log').write_text(planned)
    candidates = []
    for line in planned.splitlines():
        args = shlex.split(line)
        if (args and args[0] == str(gcc) and '-c' in args and '-MM' not in args
                and args[args.index('-c') + 1].endswith('/ph_port.c')):
            candidates.append(args)
    if len(candidates) != 1:
        raise ValueError('Cannot identify the native product compile command')
    args = candidates[0]
    args[args.index('-o') + 1] = str(output / 'ph_port.o')
    args.append('-fstack-usage')
    (output / 'command.json').write_text(json.dumps({'cwd': str(cwd), 'argv': args}, indent=2) + '\n')
    with (output / 'compiler.log').open('w') as log:
        subprocess.run(args, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
    frames = check_report((output / 'ph_port.su').read_text())
    result = {'status': 'PASS', 'limits': LIMITS, 'frames': frames,
              'scope': 'Native static frames; excludes callees and interrupt context'}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: native startup and persistence stack frame budgets')
