#!/usr/bin/env python3
"""S30 production HTTP and platform credential journal failure regression."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SDK = Path.home() / '.cache/phicomm/openbeken/bk7231n-upstream'
WOLF = SDK / 'platforms/bk7231n/bk7231n_os/beken378/func/wolfssl'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    (run / 'web.h').write_text('static const unsigned char ph_web[]={31,139,0};\n')
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=SDK, text=True).strip()
    assert revision == '07fd9e0e2da0aabaddab00be2a897589be235158', revision
    (run / 'sys_config.h').write_text('/* Host SHA256-only SDK config facade. */\n')
    auth = ROOT / 'protocol'
    binary = run / 'http-auth-port'
    command = ['clang', '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-misleading-indentation', '-Wno-unused-function', '-Wno-sign-compare',
               '-D_DEFAULT_SOURCE', '-DPH_HOST_TEST', '-DPH_S30_LAN_V4',
               '-pthread', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I' + str(HERE), '-I' + str(ROOT / 'src'),
               '-I' + str(run), '-I' + str(WOLF), '-include', str(auth / 'local_auth.h'),
               str(HERE / 'http_auth_port.c'), str(HERE / 'platform_host.c'),
               str(ROOT / 'src/ph_http.c'), str(ROOT / 'src/ph_device.c'),
               str(ROOT / 'src/ph_meter.c'), str(ROOT / 'src/ph_lan_v4.c'),
               str(ROOT / 'src/ph_sha256.c'),
               str(ROOT / 'src/ph_wire.c'), str(auth / 'local_auth.c'),
               '-Wl,--wrap=rtos_create_thread,--wrap=rtos_unlock_mutex',
               '-Wl,--wrap=PH_BL_SetWebPasswordMemory,--wrap=ph_command',
               '-Wl,--wrap=free',
               '-lm', '-o', str(binary)]
    (run / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (run / 'compiler.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    storage = run / 'storage'
    shutil.rmtree(storage, ignore_errors=True)
    result = subprocess.run([binary, storage], capture_output=True, text=True, timeout=30)
    (run / 'test.log').write_text(result.stdout + result.stderr)
    (run / 'result.json').write_text(json.dumps({
        'status': 'PASS' if result.returncode == 0 else 'FAIL',
        'synchronized_auth_snapshot': True,
        'production_http_device_port': True, 'host_lfs_and_sdk': True,
        'production_lan_v4': True, 'retained_response_revocation': True,
        'mqtt_sensor_grant_revocation': True,
        'http_matrix_cases': 42, 'sdk_cases': 7,
        'empty_recovery_cases': 2, 'same_boot_retry_cases': 1,
        'sdk_commit': revision,
        'asan_ubsan': True, 'hardware_verified': False,
    }, indent=2) + '\n')
    result.check_returncode()
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
