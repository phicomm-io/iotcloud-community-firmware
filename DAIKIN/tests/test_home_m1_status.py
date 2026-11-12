"""Production M1 status MessagePack reaches the production HomeSensor loop."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from DAIKIN.tests.arduinojson import codec_source

ROOT = Path(__file__).resolve().parents[2]


class HomeM1Status(unittest.TestCase):
    def test_production_sensor_health_age_and_hap_characteristics(self):
        with tempfile.TemporaryDirectory(prefix='hub-m1-hap-') as directory:
            build = Path(directory)
            home = (ROOT/'DAIKIN/src/home.inc').read_text()
            (build/'home-sensor.inc').write_text(
                'struct HomeSensor:' + home.split('struct HomeSensor:', 1)[1].split(
                    'static void homeAccessory(', 1)[0])
            flags = ['-g', '-O1', '-fsanitize=address,undefined',
                     '-fno-omit-frame-pointer', '-fno-pie']
            objects = []
            for i, source in enumerate(('device_status.c', 'typed_wire.c', 'wire_buffer.c')):
                obj = build/(str(i)+'.o')
                subprocess.run(['cc', '-std=c11', *flags, '-DPHICOMM_STATUS_MODEL=1',
                                '-DPHICOMM_APPLICATION_WATCHDOG=1',
                                '-DPHICOMM_FULL_RESOURCES=1', '-I'+str(ROOT/'common'),
                                '-I'+str(ROOT/'tests/host'), '-c',
                                str(ROOT/'common'/source), '-o', str(obj)], check=True)
                objects.append(str(obj))
            binary = build/'test'
            subprocess.run(['c++', '-std=c++11', *flags, '-no-pie', '-Wall', '-Wextra',
                            '-Werror', '-Wno-misleading-indentation',
                            '-Wno-missing-field-initializers',
                            '-I'+str(build), '-I'+str(ROOT/'DAIKIN/include'),
                            '-I'+str(codec_source()),
                            str(ROOT/'DAIKIN/tests/hub/home_m1_status.cpp'),
                            *objects, '-lcrypto', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=15,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                                'UBSAN_OPTIONS': 'halt_on_error=1'})


if __name__ == '__main__':
    unittest.main()
