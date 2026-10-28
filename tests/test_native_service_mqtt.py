"""Native shared service through the pinned Paho MQTT client."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeServicePaho(unittest.TestCase):
    """Delegation targets for the shared service fixtures in test_shared_mqtt."""

    def _compile_fixture(self, defines=(), client_source=None):
        from tests.test_shared_mqtt import SharedMQTT

        with SharedMQTT().protocol('A1') as protocol, tempfile.TemporaryDirectory() as temp:
            temporary = Path(temp)
            generated = temporary / 'generated'
            generated.mkdir()
            (generated / 'build_ota.h').write_text('')
            binary = temporary / 'service-test'
            source = [ROOT / 'tests/mqtt_core/service_mp.c',
                      ROOT / 'common/mqtt/core.c',
                      Path(client_source) if client_source else ROOT / 'common/mqtt/client.c',
                      ROOT / 'common/mqtt/session.c',
                      protocol / 'MQTTClient.c',
                      *sorted((protocol / 'MQTTPacket').glob('*.c')),
                      ROOT / 'common/typed_wire.c', ROOT / 'common/wire_buffer.c']
            command = [os.environ.get('CC', 'cc'), '-D_GNU_SOURCE', '-std=gnu99', '-g',
                       '-fsanitize=' + os.environ.get('PHICOMM_MQTT_SANITIZER', 'address,undefined'),
                       '-fno-pie', '-no-pie', '-I' + str(temporary),
                       '-I' + str(ROOT / 'common/mqtt'),
                       '-I' + str(ROOT / 'tests/mqtt_core/stubs'),
                       '-I' + str(ROOT / 'tests/service_mp'), '-I' + str(ROOT)]
            command += ['-I' + str(path) for path in
                        (protocol, protocol / 'mico', protocol / 'MQTTPacket')]
            command += ['-include', 'stdint.h', '-DMOC=1',
                        '-DMAX_MESSAGE_HANDLERS=12', '-DPH_EXPECTED_MESSAGE_HANDLERS=12',
                        '-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup']
            command += list(defines) + [str(path) for path in source]
            command += ['-lcrypto', '-lm', '-o', str(binary)]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=30)

    def run_actual_service(self):
        """Exercise the actual shared dispatcher and admission/error paths."""
        self._compile_fixture()

    def run_large_receipt_during_dispatch(self):
        """Exercise real Paho TX resize, receipt replay and the callback gate."""
        self._compile_fixture(defines=['-DLARGE_RECEIPT_FIXTURE'])

        # A legitimate receipt is published after dispatch unwinds but while
        # still inside the Paho callback. The old callback-only gate rejected it.
        source = (ROOT / 'common/mqtt/client.c').read_text()
        original = ('c == ctx->client && ctx->core.protocol_depth &&\n'
                    '        permit(NULL, bytes)')
        blocked = source.replace(
            original,
            'c == ctx->client && ctx->core.protocol_depth &&\n'
            '        !ctx->core.dispatch_depth && permit(NULL, bytes)')
        self.assertNotEqual(source, blocked)
        with tempfile.TemporaryDirectory() as directory:
            client = Path(directory) / 'client.c'
            client.write_text(blocked)
            with self.assertRaises(subprocess.CalledProcessError):
                self._compile_fixture(defines=['-DLARGE_RECEIPT_FIXTURE'],
                                      client_source=client)

    def run_preflight_receipts(self):
        """Cover owned receipt pressure, cancel, expiry, rename and stop."""
        self._compile_fixture(defines=['-DPREFLIGHT_FIXTURE'])

    def test_actual_service_dispatch(self):
        self.run_actual_service()

    def test_large_service_receipt_during_dispatch(self):
        self.run_large_receipt_during_dispatch()

    def test_preflight_service_receipt_lifecycle(self):
        self.run_preflight_receipts()


if __name__ == '__main__':
    unittest.main()
