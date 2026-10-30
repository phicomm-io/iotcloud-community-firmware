"""Protocol-neutral hub application routing, without embedded MQTT broker code."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DeviceBus(unittest.TestCase):
    def test_business_routes_without_broker(self):
        codec = Path(os.environ.get('DAIKIN_ARDUINOJSON_SOURCE',
                                    str(ROOT / '.pio/libdeps/bry88ab-hub/ArduinoJson/src')))
        with tempfile.TemporaryDirectory(prefix='hub-device-bus-') as directory:
            binary = Path(directory) / 'device-bus'
            subprocess.run([
                'c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I' + str(ROOT / 'include'), '-I' + str(codec),
                str(ROOT / 'tests/hub/device_bus.cpp'), '-o', str(binary)
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=30,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                                'UBSAN_OPTIONS': 'halt_on_error=1'})
