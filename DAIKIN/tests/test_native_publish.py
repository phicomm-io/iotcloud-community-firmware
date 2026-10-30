"""Production business status event cache and numbered diagnostics.

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
        (temp / 'freertos').mkdir()
        (temp / 'freertos/semphr.h').write_text('''#pragma once
#include <mutex>
#include <chrono>
typedef std::timed_mutex *SemaphoreHandle_t;
static const int pdTRUE=1;
static std::timed_mutex snapshotMutex;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(){return &snapshotMutex;}
static inline int xSemaphoreTake(SemaphoreHandle_t m,unsigned ticks){return ticks?m->try_lock_for(std::chrono::milliseconds(ticks)):m->try_lock();}
static inline void xSemaphoreGive(SemaphoreHandle_t m){m->unlock();}
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
        (temp / 'state.inc').write_text('#include "diagnostics_ids.h"\n' + section(src / 'commands.inc', 'static void otaPartitionStatus(', 'static const char *actions'))
        (temp / 'public_read.inc').write_text(section(src / 'http.inc', 'static bool publicReadCommand(', 'static esp_err_t commandHandler('))
        (temp / 'account_scope.inc').write_text(section(src / 'mqtt.inc', 'static String mqttAccountScope(', 'static void discovery('))
        shutil.copyfile(src / 'diagnostics_ids.h', temp / 'diagnostics_ids.h')
        shutil.copyfile(src / 'indicators.inc', temp / 'indicators.inc')
        cls.binary = temp / 'native-publish'
        objects=[]
        for name in ('typed_wire','wire_buffer'):
            obj=temp/(name+'.o');objects.append(str(obj))
            subprocess.run(['cc','-std=gnu99','-g','-O1','-fsanitize=address,undefined','-fno-pie',
                '-c',str(ROOT/'common'/(name+'.c')),'-o',str(obj)],check=True)
        subprocess.run([
            'c++', '-std=c++11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-copy', '-Wno-unused-function', '-Wno-sign-compare',
            '-fno-exceptions', '-ffunction-sections', '-fdata-sections',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1',
            '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0', '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0',
            '-DARDUINOJSON_ENABLE_PROGMEM=0', '-DIOTCLOUD_HUB=1',
            '-I' + str(temp), '-I' + str(ROOT / 'DAIKIN/include'), '-I' + str(codec), '-I'+str(ROOT/'common'),
            str(ROOT / 'DAIKIN/tests/hub/status_cache.cpp'), str(temp / 'WString.cpp'),
            *objects, '-Wl,--gc-sections', '-pthread',
            '-o', str(cls.binary)], check=True)

    def scenario(self, name):
        subprocess.run([str(self.binary), name], check=True, timeout=30,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                            'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})

    def test_production_event_cache_and_numbered_diagnostics(self):
        subprocess.run([str(self.binary)],check=True,timeout=30,
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})

if __name__ == '__main__':
    unittest.main()
