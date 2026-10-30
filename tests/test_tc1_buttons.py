"""TC1 physical sampling through native control under MQTT/heap failures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_tc1_sdk_migration import function

ROOT = Path(__file__).resolve().parents[1]


class TC1Buttons(unittest.TestCase):
    def test_offline_and_low_heap_controls(self):
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT/'build/tc1-sdk-3.6.4/mico-os'))
        sha = sdk/'MiCO/security/SHAUtils'
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            gpio = (ROOT/'TC1/user_gpio.c').read_text()
            mqtt = (ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text()
            functions = '\n'.join(function(gpio, signature) for signature in (
                'bool RelayOut(', 'void UserLedRefresh(', 'void UserLedSet(',
                'void UserRelaySet(', 'void UserRelaySetAll(', 'static void KeyTimeoutHandler('
            ))
            functions += function(mqtt, 'void UserMqttRefreshState(')
            (out/'tc1_button_functions.inc').write_text(functions)
            command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-g',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                       '-I'+str(ROOT/'tests/stubs'), '-I'+str(ROOT),
                       '-I'+str(out), '-I'+str(sha),
                       str(ROOT/'tests/tc1_buttons_harness.c'),
                       str(ROOT/'common/typed_wire.c'), str(ROOT/'common/wire_buffer.c'),
                       str(sha/'sha224-256.c'),
                       '-o', str(out/'buttons')]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(out/'buttons')], capture_output=True,
                                    text=True, timeout=10,
                                    env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS TC1 gestures:'), 12)
            self.assertIn('PASS TC1 admission, persistence rollback', result.stdout)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
