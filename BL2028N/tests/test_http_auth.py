#!/usr/bin/env python3
"""S30 production HTTP/device integration with its synchronized auth snapshot."""
import argparse
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    (run / 'web.h').write_text('static const unsigned char ph_web[]={31,139,0};\n')
    auth_root = ROOT / 'protocol'
    binary = run / 'http-auth'
    command = ['clang', '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-misleading-indentation', '-Wno-unused-function', '-Wno-sign-compare',
               '-D_DEFAULT_SOURCE', '-DPH_HOST_TEST', '-pthread', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I' + str(HERE), '-I' + str(ROOT / 'src'),
               '-I' + str(run), '-include', str(auth_root / 'local_auth.h'),
               str(HERE / 'http_auth.c'), str(ROOT / 'src/ph_http.c'),
               str(ROOT / 'src/ph_device.c'), str(ROOT / 'src/ph_meter.c'),
               str(ROOT / 'src/ph_wire.c'), str(auth_root / 'local_auth.c'),
               '-Wl,--wrap=ph_auth_internal_context', '-lm', '-o', str(binary)]
    with (run / 'compiler.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, timeout=30)
    (run / 'test.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
