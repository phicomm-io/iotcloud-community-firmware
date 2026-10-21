"""Actual shared queue and pinned Paho protocol; transport is in-memory, no sockets."""
import os
import io
import tarfile
from contextlib import contextmanager
from tools.apply_sdk_patch import apply_stack
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_http_json_owned import copy_patched_json
ROOT = Path(__file__).resolve().parents[1]
class SharedMQTT(unittest.TestCase):
    @contextmanager
    def protocol(self, model):
        name = 'tc1-sdk-3.6.4' if model == 'TC1' else model.lower() + '-sdk'
        sdk = Path(os.environ.get('PHICOMM_' + model + '_SDK', ROOT / 'build' / name / 'mico-os'))
        self.assertTrue((sdk / 'libraries/protocols/mqtt/MQTTClient.c').is_file(),
                        model + ' pinned SDK required; run tools/prepare_sdk.py')
        pin = subprocess.check_output(['python3', str(ROOT/'tools/prepare_sdk.py'), '--commit'], text=True).strip()
        actual = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip()
        self.assertEqual(actual, pin, 'protocol fixture must use the production SDK pin')
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory)
            archive = subprocess.check_output(['git', '-C', str(sdk), 'archive', 'HEAD', 'libraries/protocols/mqtt'])
            with tarfile.open(fileobj=io.BytesIO(archive)) as source:
                source.extractall(copy, filter='data')
            if model != 'TC1':
                # Preserve each product's real Yield semantics. Other board files
                # in sdk.patch are deliberately outside this isolated fixture.
                parts = (ROOT/model/'sdk.patch').read_bytes().split(b'diff --git ')
                patch = copy/'product.patch'
                patch.write_bytes(b''.join(b'diff --git '+part for part in parts[1:]
                    if part.startswith(b'a/libraries/protocols/mqtt/')))
                if patch.stat().st_size:
                    apply_stack(copy, [patch])
            apply_stack(copy, [ROOT/p for p in ('common/mqtt-mico36.patch',
                'common/mqtt-publish-bounds.patch', 'common/mqtt-socket-close.patch',
                'M1/mqtt-packet-deadline.patch', 'common/mqtt-response-deadline.patch')])
            yield copy/'libraries/protocols/mqtt'

    def compile_run(self, sources, includes=(), defines=()):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'test'
            command = [os.environ.get('CC', 'cc'), '-D_GNU_SOURCE', '-std=gnu99', '-g',
                       '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                       '-I' + str(ROOT / 'common/mqtt')]
            command += ['-I' + str(p) for p in includes]
            command += list(defines) + [str(p) for p in sources] + ['-o', str(binary)]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=30)
    def test_core(self):
        self.compile_run([ROOT / p for p in ('tests/mqtt_core/core.c', 'common/mqtt/core.c')])
    def test_concurrent_clear(self):
        self.compile_run([ROOT/"tests/mqtt_core/concurrent.c", ROOT/"common/mqtt/core.c"], defines=["-pthread"])
    def check_protocol(self, model):
        with self.protocol(model) as protocol:
            self.compile_run([ROOT / 'tests/mqtt_core/paho.c', ROOT / 'common/mqtt/core.c',
                              ROOT / 'common/mqtt/client.c', ROOT / 'common/mqtt/session.c', protocol / 'MQTTClient.c',
                              *sorted((protocol / 'MQTTPacket').glob('*.c'))],
                             [ROOT / 'tests/mqtt_core/stubs', protocol, protocol / 'mico', protocol / 'MQTTPacket'],
                             ['-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])
    def test_actual_service(self):
        sdk = ROOT/'build/a1-sdk/mico-os'
        protocol=sdk/'libraries/protocols/mqtt'
        with self.protocol('A1') as protocol, tempfile.TemporaryDirectory() as directory:
            json=copy_patched_json(sdk/'libraries/utilities/json_c',Path(directory)/'sdk')
            for p in json.glob('*.c'): p.write_text(p.read_text().replace('%ld','%d'))
            bear=ROOT/'build/ota-tls/bearssl-0.6'
            self.compile_run([ROOT/'tests/mqtt_core/service.c',ROOT/'common/mqtt/core.c',
                ROOT/'common/mqtt/client.c',ROOT/'common/mqtt/session.c',ROOT/'common/mqtt/service.c',
                ROOT/'common/json_owned.c', protocol/'MQTTClient.c',
                *sorted((protocol/'MQTTPacket').glob('*.c')), *json.glob('*.c'),
                *[bear/'src'/p for p in ('hash/sha2small.c','codec/dec32be.c','codec/enc32be.c','codec/enc64be.c')]],
                [ROOT/'tests/mqtt_core/stubs',ROOT/'tests/stubs',protocol,protocol/'mico',protocol/'MQTTPacket',json,bear/'inc',bear/'src'],
                ['-include','stdint.h','-DMOC=1','-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])
    def test_a1_protocol(self): self.check_protocol('A1')
    def test_m1_protocol(self): self.check_protocol('M1')
    def test_tc1_protocol(self): self.check_protocol('TC1')
if __name__ == '__main__': unittest.main()
