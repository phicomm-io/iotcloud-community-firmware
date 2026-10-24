"""TC1 physical sampling through native control under MQTT/heap failures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json
from tests.test_tc1_sdk_migration import function

ROOT = Path(__file__).resolve().parents[1]


class TC1Buttons(unittest.TestCase):
    def test_offline_and_low_heap_controls(self):
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT/'build/tc1-sdk-3.6.4/mico-os'))
        bear = ROOT/'build/ota-tls/bearssl-0.6'
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            json = copy_patched_json(sdk/'libraries/utilities/json_c', out/'sdk')
            # Native long is 64 bits; the pinned firmware library targets ILP32.
            for source in json.glob('*.c'):
                source.write_text(source.read_text().replace('%ld', '%d'))
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
                       '-I'+str(ROOT/'tests/stubs'), '-I'+str(ROOT), '-I'+str(json),
                       '-I'+str(out), '-I'+str(bear/'inc'), '-I'+str(bear/'src'),
                       str(ROOT/'tests/tc1_buttons_harness.c'), str(ROOT/'common/json_owned.c'),
                       *map(str, sorted(json.glob('*.c'))),
                       *[str(bear/name) for name in ('src/hash/sha2small.c',
                           'src/codec/dec32be.c', 'src/codec/enc32be.c', 'src/codec/enc64be.c')],
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
