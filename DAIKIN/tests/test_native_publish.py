"""Normal native publication with production JSON, ESP32 String and Broker code.

No allocator failures or protocol faults are injected. The board/SDK I/O edges
use fixed normal values; WString.cpp is copied unchanged into the temporary
host build so its quoted Arduino includes can resolve to the host adapters.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PIO = Path(os.environ.get('PLATFORMIO_CORE_DIR', str(Path.home() / '.platformio')))
ARDUINO = Path(os.environ.get('DAIKIN_ARDUINO_SOURCE',
    str(PIO / 'packages/framework-arduinoespressif32')))


def section(path, start, end):
    source = path.read_text()
    return start + source.split(start, 1)[1].split(end, 1)[0]


class NativePublish(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='daikin-native-publish-')
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        core = ARDUINO / 'cores/esp32'
        codec = ROOT / 'DAIKIN/.pio/libdeps/bry88ab-hub/ArduinoJson/src'
        if not (core / 'WString.cpp').is_file() or not (codec / 'ArduinoJson.h').is_file():
            raise RuntimeError('Install the production PlatformIO hub dependencies first')
        version = (codec / 'ArduinoJson/version.hpp').read_text()
        if '#define ARDUINOJSON_VERSION "6.21.5"' not in version:
            raise RuntimeError('These regressions require pinned ArduinoJson 6.21.5')
        for name in ('WString.cpp', 'WString.h'):
            shutil.copyfile(core / name, temp / name)
        (temp / 'Arduino.h').write_text('''#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "WString.h"
''')
        (temp / 'pgmspace.h').write_text('''#pragma once
#include <cstring>
#define PSTR(s) (s)
#define memcpy_P std::memcpy
''')
        (temp / 'esp32-hal-log.h').write_text('''#pragma once
#define log_e(...) ((void)0)
#define log_w(...) ((void)0)
''')
        # Numeric constructors are unused and removed by gc-sections. Keep
        # declarations for parsing the unchanged SDK translation unit.
        (temp / 'stdlib_noniso.h').write_text('''#pragma once
extern "C" {
char *itoa(int,char*,int); char *ltoa(long,char*,int);
char *lltoa(long long,char*,int); char *utoa(unsigned int,char*,int);
char *ultoa(unsigned long,char*,int); char *ulltoa(unsigned long long,char*,int);
char *dtostrf(double,int,unsigned int,char*);
}
''')
        src = ROOT / 'DAIKIN/src'
        extracts = {
            'allocation.inc': section(src / 'hub.inc', 'static size_t hubByteHeap()', 'static bool hubReload'),
            'mqtt.inc': section(src / 'mqtt.inc', 'static void publish(', 'static void discovery('),
            'native.inc': section(src / 'hub.inc', 'static void hubLocalPublish(', 'static void hubInit('),
            'ready.inc': section(src / 'hub.inc', 'static bool hubReady()', 'static void hubApiExecute('),
            'state.inc': section(src / 'commands.inc', 'static void otaPartitionStatus(', 'static const char *actions'),
            'typed.inc': section(src / 'sessions.inc', 'static void sensorBody(', 'static void sessionStatus('),
            'indicators.inc': (src / 'indicators.inc').read_text(),
            'status-work.inc': section(src / 'main.cpp', 'if(webOperation==4){', 'else webResult=otaActive?'),
            'status-handler.inc': section(src / 'http.inc', 'static esp_err_t statusHandler(', 'static esp_err_t optionsHandler('),
            'json-response.inc': section(src / 'http.inc', 'static esp_err_t jsonResponse(', 'static bool authorized('),
        }
        for name, source in extracts.items():
            (temp / name).write_text(source)
        cls.binary = temp / 'native-publish'
        subprocess.run([
            'c++', '-std=c++11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-copy', '-Wno-unused-function', '-Wno-sign-compare',
            '-fno-exceptions', '-ffunction-sections', '-fdata-sections',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1',
            '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0', '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0',
            '-DARDUINOJSON_ENABLE_PROGMEM=0', '-DIOTCLOUD_HUB=1',
            '-I' + str(temp), '-I' + str(ROOT / 'DAIKIN/include'), '-I' + str(codec),
            str(ROOT / 'DAIKIN/tests/hub/native_publish.cpp'), str(temp / 'WString.cpp'),
            '-Wl,--gc-sections', '-Wl,--wrap=malloc', '-Wl,--wrap=realloc', '-Wl,--wrap=free',
            '-o', str(cls.binary)], check=True)

    def scenario(self, name):
        subprocess.run([str(self.binary), name], check=True, timeout=30,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                            'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})

    def test_state_and_typed_namespaces_preserve_normal_payloads(self):
        self.scenario('namespaces')

    def test_ha_config_rewrites_all_references_for_long_legal_usernames(self):
        self.scenario('discovery')

    def test_empty_retained_native_message_deletes_the_real_broker_snapshot(self):
        self.scenario('retained')

    def test_measured_rendering_appends_without_further_allocations(self):
        self.scenario('render')

    def test_actual_state_then_typed_publish_release_json_and_first_wire(self):
        self.scenario('state')

    def test_repeated_normal_publication_preserves_complete_sensor_snapshots(self):
        self.scenario('repeat')

    def test_account_readiness_does_not_construct_owning_strings(self):
        self.scenario('ready')


if __name__ == '__main__':
    unittest.main()
