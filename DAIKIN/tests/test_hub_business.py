"""Transport-neutral account, HAP model and cloud/Bemfa business regressions.

This does not validate mDNS discovery, authenticated UDP, sockets or a device.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class HubBusiness(unittest.TestCase):
    def scenario(self, name):
        codec = Path(os.environ.get('DAIKIN_ARDUINOJSON_SOURCE',
                                    str(ROOT / '.pio/libdeps/bry88ab-hub/ArduinoJson/src')))
        with tempfile.TemporaryDirectory(prefix='hub-business-') as directory:
            binary = Path(directory) / name
            subprocess.run([
                'c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I' + str(ROOT / 'include'), '-I' + str(codec),
                str(ROOT / 'tests/hub' / (name + '.cpp')), '-lcrypto', '-o', str(binary)
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=30,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                                'UBSAN_OPTIONS': 'halt_on_error=1'})

    def test_account_configuration_isolation(self):
        self.scenario('service')

    def test_homekit_model_ack_then_state(self):
        self.scenario('home')

    def test_cloud_routes_and_bemfa(self):
        self.scenario('relay')
