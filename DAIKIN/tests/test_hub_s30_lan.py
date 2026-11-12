"""Real S30 endpoint + production hub password-pairing and v4 business sender."""
import argparse
import os
from pathlib import Path
import re
import select
import shlex
import subprocess
import tempfile
import unittest

from DAIKIN.tests.arduinojson import codec_source

ROOT = Path(__file__).resolve().parents[2]


def build_client(build):
    build = Path(build)
    build.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'DAIKIN/src/hub_lan.inc').read_text()
    sections = []
    for start, end in (
        ('static bool hubLanRegistered(', 'static bool hubLanPaired('),
        ('static bool hubLanPaired(', 'static void hubLanStatus('),
        ('static bool hubLanRouteConfirmed(const iotcloud::HubLanDiscovery::Peer &peer){',
         'static void hubLanReset('),
        ('static void hubLanObserveDiscovery(', '// A transport failure is a correlated result,'),
    ):
        sections.append(source[source.index(start):source.index(end, source.index(start))])
    (build / 'hub-s30-gates.inc').write_text('\n'.join(sections))
    start = source.index('  hubLanClient.authorize=[]')
    end = source.index('  hubLanClient.send=[]', start)
    (build / 'hub-s30-authorize.inc').write_text(source[start:end])
    (build / 'wire.c').write_text('#include "lan_wire_v4.inc"\n')
    command_log = build / 'compiler-command.txt'
    compiler_log = build / 'compiler.log'
    command_log.write_text('')
    compiler_log.write_text('')

    def compile_command(command):
        with command_log.open('a') as log:
            log.write(shlex.join(command) + '\n')
        with compiler_log.open('a') as log:
            try:
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            except subprocess.CalledProcessError:
                print(compiler_log.read_text(), end='')
                raise

    flags = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
    objects = []
    for name, path in (
        ('typed', ROOT / 'common/typed_wire.c'), ('buffer', ROOT / 'common/wire_buffer.c'),
        ('auth', ROOT / 'common/local_auth.c'), ('wire', build / 'wire.c'),
    ):
        obj = build / (name + '.o')
        compile_command(['cc', '-std=c11', *flags, '-I' + str(ROOT / 'common'),
                         '-c', str(path), '-o', str(obj)])
        objects.append(str(obj))
    binary = build / 'hub-s30-lan'
    compile_command([
        'c++', '-std=c++11', *flags, '-no-pie', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-misleading-indentation',
        '-I' + str(ROOT / 'DAIKIN/include'), '-I' + str(ROOT / 'common'),
        '-I' + str(codec_source()), '-I' + str(build),
        str(ROOT / 'DAIKIN/tests/hub/s30_lan.cpp'), *objects, '-lcrypto', '-o', str(binary),
    ])
    return binary


def run_interop(endpoint, build):
    binary = build_client(build)
    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}
    with subprocess.Popen([str(endpoint), '0'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, env=env) as process:
        failure = None
        try:
            if not select.select([process.stdout], [], [], 10)[0]:
                raise TimeoutError('S30 host did not announce a port within 10 seconds')
            announcement = process.stdout.readline().strip()
            match = re.fullmatch(r'PORT (\d+) ID ([a-f0-9]{12}) PASSWORD (\S+)', announcement)
            if not match:
                raise RuntimeError('S30 host startup failed: ' + announcement)
            subprocess.run([str(binary), *match.groups()], check=True, timeout=45, env=env)
        except BaseException as error:
            failure = error
            raise
        finally:
            if process.poll() is None:
                try:
                    process.stdin.write('STOP\n')
                    process.stdin.flush()
                except BrokenPipeError:
                    pass
            try:
                output, error = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                output, error = process.communicate(timeout=10)
            if output.strip():
                print(output.strip())
            if error:
                print(error, end='')
            if process.returncode and failure is None:
                raise RuntimeError('S30 host exited with status ' + str(process.returncode))


class HubS30Lan(unittest.TestCase):
    def test_production_endpoint_pair_resume_command_pull_ack(self):
        # The endpoint is built by the S30 suite from its actual typed business
        # and common auth/core. It is never replaced by a fixed-token server.
        from BL2028N.tests.test_lan_v4 import build_host
        with tempfile.TemporaryDirectory(prefix='hub-s30-real-lan-') as temporary:
            build = Path(temporary)
            run_interop(build_host(build / 's30'), build / 'hub')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--endpoint', type=Path)
    parser.add_argument('--run-dir', type=Path)
    args, remaining = parser.parse_known_args()
    if args.endpoint:
        if not args.run_dir:
            parser.error('--endpoint requires --run-dir')
        run_interop(args.endpoint, args.run_dir)
    else:
        unittest.main(argv=[__file__, *remaining])
