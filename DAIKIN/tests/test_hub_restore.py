"""Run production restore/reset handlers and quiesce hooks with live MQTT sockets."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def section(path, start, end=None):
    text = (ROOT / path).read_text()
    result = start + text.split(start, 1)[1]
    return result.split(end, 1)[0] if end else result


def port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


class HubRestore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='daikin-hub-restore-')
        root = Path(cls.tmp.name)
        parts = {
            'save.inc': section('DAIKIN/src/main.cpp', 'static bool saveConfig(', 'static void startAccessPoint('),
            'backup.inc': section('DAIKIN/src/http.inc', 'static String backupExecute()', 'static esp_err_t backupHandler('),
            'backup-handler.inc': section('DAIKIN/src/http.inc', 'static esp_err_t backupHandler(', 'static volatile bool otaActive'),
            'json-response.inc': section('DAIKIN/src/http.inc', 'static esp_err_t jsonResponse(', 'static bool authorized('),
            'commands.inc': section('DAIKIN/src/commands.inc', 'static bool knownFields('),
            'hub-globals.inc': section('DAIKIN/src/hub.inc', 'static bool hubReload', 'static String hubHex('),
            'mqtt-stop.inc': section('DAIKIN/src/mqtt.inc', 'static void stopMqtt()', 'static void startMqtt('),
            'bemfa-stop.inc': section('DAIKIN/src/hub.inc', 'static void stopBemfa()', 'static void startBemfa('),
            'quiesce.inc': section('DAIKIN/src/hub.inc', 'static bool hubRestartPending()', 'static void hubLocalPublish('),
            'poll.inc': section('DAIKIN/src/hub.inc', 'static void pollHub('),
            'home-quiesce.inc': section('DAIKIN/src/home.inc', 'static void homeQuiesce()', 'static void homeChanged('),
        }
        for name, text in parts.items():
            (root / name).write_text(text)
        includes = [ROOT / 'DAIKIN/.pio/libdeps/bry88ab-hub/ArduinoJson/src',
                    ROOT / 'DAIKIN/.pio/libdeps/bry88ab/ArduinoJson/src']
        codec = next((p for p in includes if (p / 'ArduinoJson.h').exists()), None)
        if codec is None:
            raise RuntimeError('Build one DAIKIN PlatformIO target to fetch the pinned ArduinoJson dependency')
        cls.binary = root / 'restore'
        subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I' + str(root), '-I' + str(ROOT / 'DAIKIN/include'), '-I' + str(codec),
                        '-I' + str(ROOT / 'common'), str(ROOT / 'DAIKIN/tests/hub/restore.cpp'),
                        '-lcrypto', '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scenario(self, name):
        tcp, ws = port(), port()
        while tcp == ws:
            ws = port()
        subprocess.run([str(self.binary), name, str(tcp), str(ws)], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'}, timeout=15)

    def test_restore_new_owner_immediately_revokes_old_access(self):
        self.scenario('restore')

    def test_restore_same_home_identity_still_quiesces(self):
        self.scenario('same')

    def test_failed_or_invalid_restore_preserves_old_account(self):
        self.scenario('restore-fail')

    def test_factory_reset_revokes_old_access_and_blocks_config_rewrite(self):
        self.scenario('reset')

    def test_failed_factory_reset_never_reports_accepted(self):
        self.scenario('reset-fail')

    def test_native_mqtt_config_commit_retires_old_client_before_return(self):
        self.scenario('mqtt')

    def test_native_mqtt_config_from_local_broker_waits_for_complete_reload(self):
        self.scenario('mqtt-local')

    def test_failed_native_mqtt_config_keeps_old_client_and_inbox(self):
        self.scenario('mqtt-fail')

    def test_backup_get_under_fragmented_heap_returns_complete_backup(self):
        self.scenario('backup-fit')

    def test_backup_allocation_or_short_serialization_is_503(self):
        for failure in ('json', 'string', 'short'):
            with self.subTest(failure=failure):
                self.scenario('backup-' + failure)

    def test_restore_oom_is_503_and_preserves_live_account_and_storage(self):
        for failure in ('parse', 'candidate', 'body', 'concat'):
            with self.subTest(failure=failure):
                self.scenario('restore-oom-' + failure)

    def test_restore_larger_valid_backup_fits_fragmented_heap(self):
        self.scenario('restore-fit')


if __name__ == '__main__':
    unittest.main()
