"""Exercise production SDK-client reload ordering and inbox generation changes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def section(path, start, end=None):
    text = (ROOT / path).read_text()
    result = start + text.split(start, 1)[1]
    return result.split(end, 1)[0] if end else result


class HubReload(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='daikin-hub-reload-')
        root = Path(cls.tmp.name)
        parts = {
            'globals.inc': section('DAIKIN/src/hub.inc', 'static bool hubReload', 'static String hubHex('),
            'capture.inc': section('DAIKIN/src/hub.inc', 'static bool hubCapture(', 'static void hubSubscribe('),
            'mqtt.inc': section('DAIKIN/src/mqtt.inc', 'static void stopMqtt()'),
            'bemfa.inc': section('DAIKIN/src/hub.inc', 'static void stopBemfa()', 'static bool hubRestartPending()'),
            'inbox.inc': section('DAIKIN/src/hub.inc', 'static void hubResetInbox()', 'static void hubRestoreChanged()'),
            'poll.inc': section('DAIKIN/src/hub.inc', 'static void pollHub('),
        }
        for name, text in parts.items():
            (root / name).write_text(text)
        includes = [ROOT / 'DAIKIN/.pio/libdeps/bry88ab-hub/ArduinoJson/src',
                    ROOT / 'DAIKIN/.pio/libdeps/bry88ab/ArduinoJson/src']
        codec = next((p for p in includes if (p / 'ArduinoJson.h').exists()), None)
        if codec is None:
            raise RuntimeError('Build one DAIKIN target to fetch the pinned ArduinoJson dependency')
        cls.binary = root / 'reload'
        subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I' + str(root), '-I' + str(ROOT / 'DAIKIN/include'), '-I' + str(codec),
                        str(ROOT / 'DAIKIN/tests/hub/reload.cpp'), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scenario(self, name):
        subprocess.run([str(self.binary), name], check=True, timeout=15,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})

    def test_old_clients_and_fragments_cannot_cross_route_generation(self):
        self.scenario('reload')

    def test_startup_without_existing_clients_and_subsequent_reload(self):
        self.scenario('startup')

    def test_native_mqtt_change_retires_old_source_before_new_generation(self):
        self.scenario('native')


if __name__ == '__main__':
    unittest.main()
