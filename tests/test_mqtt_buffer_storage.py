"""Exercise the pinned TC1 MQTT client against allocator-visible capacities."""
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

from tools.apply_sdk_patch import apply_stack

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('PHICOMM_TC1_SDK', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))


class MQTTBufferStorage(unittest.TestCase):
    def protocol(self, directory, with_fix):
        pin = subprocess.check_output(
            ['python3', str(ROOT / 'tools/prepare_sdk.py'), '--commit'], text=True
        ).strip()
        actual = subprocess.check_output(
            ['git', '-C', str(SDK), 'rev-parse', 'HEAD'], text=True
        ).strip()
        self.assertEqual(actual, pin, 'TC1 protocol fixture must use the production SDK pin')
        archive = subprocess.check_output(
            ['git', '-C', str(SDK), 'archive', 'HEAD', 'libraries/protocols/mqtt']
        )
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            source.extractall(directory, filter='data')
        patches = [
            'common/mqtt-mico36.patch',
            'common/mqtt-publish-bounds.patch',
            'common/mqtt-socket-close.patch',
            'M1/mqtt-packet-deadline.patch',
            'common/mqtt-response-deadline.patch',
        ]
        if with_fix:
            patches.append('common/mqtt-buffer-storage.patch')
        apply_stack(directory, [ROOT / patch for patch in patches])
        return directory / 'libraries/protocols/mqtt'

    def compile_and_run(self, protocol, negative_control):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'mqtt-buffer-storage'
            command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-g',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-fno-pie', '-no-pie',
                       '-DNEGATIVE_CONTROL=' + ('1' if negative_control else '0')]
            for include in (ROOT / 'tests/mqtt_core/stubs', protocol,
                            protocol / 'mico', protocol / 'MQTTPacket'):
                command += ['-I' + str(include)]
            command += [str(ROOT / 'tests/mqtt_buffer_storage_harness.c'),
                        str(protocol / 'MQTTClient.c')]
            command += [str(path) for path in sorted((protocol / 'MQTTPacket').glob('*.c'))]
            command += ['-Wl,--wrap=malloc', '-Wl,--wrap=free', '-Wl,--wrap=realloc',
                        '-Wl,--wrap=calloc', '-o', str(binary)]
            subprocess.run(command, check=True, timeout=60)
            env = os.environ.copy()
            env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
            env['UBSAN_OPTIONS'] = 'halt_on_error=1'
            subprocess.run([str(binary)], check=True, timeout=30, env=env)

    def test_original_stack_reproduces_hidden_shrink_capacity(self):
        with tempfile.TemporaryDirectory() as temp:
            protocol = self.protocol(Path(temp), with_fix=False)
            self.compile_and_run(protocol, negative_control=True)

    def test_fixed_stack_tracks_real_tx_rx_storage(self):
        with tempfile.TemporaryDirectory() as temp:
            protocol = self.protocol(Path(temp), with_fix=True)
            self.compile_and_run(protocol, negative_control=False)


if __name__ == '__main__':
    unittest.main()
