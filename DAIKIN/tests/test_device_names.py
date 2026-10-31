"""The production DAIKIN MessagePack name.set handler and storage boundary."""
import os
from pathlib import Path
import subprocess
from . import test_native_publish as fixture

ROOT=fixture.ROOT
class DeviceNames(fixture.NativePublish):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        build=Path(cls.temp.name)
        source=(ROOT/'DAIKIN/src/commands.inc').read_text()
        (build/'commands-execute.inc').write_text('static const char *actions' + source.split('static const char *actions',1)[1])
        objects=[str(build/(name+'.o')) for name in ('typed_wire','wire_buffer')]
        cls.binary=build/'device-names'
        subprocess.run(['c++','-std=c++11','-g','-O1','-Wall','-Wextra','-Werror',
            '-Wno-deprecated-copy','-Wno-unused-function','-Wno-sign-compare','-Wno-return-type',
            '-ffunction-sections','-fdata-sections','-fno-exceptions','-fsanitize=address,undefined',
            '-fno-omit-frame-pointer','-fno-pie','-no-pie','-DARDUINOJSON_ENABLE_ARDUINO_STRING=1',
            '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0','-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0',
            '-DARDUINOJSON_ENABLE_PROGMEM=0','-DIOTCLOUD_HUB=1',
            '-I'+str(build),'-I'+str(ROOT/'DAIKIN/include'),'-I'+str(fixture.codec_source()),
            '-I'+str(ROOT/'common'),'-I'+str(ROOT/'DAIKIN/lib/HomeSpan/src'),
            str(ROOT/'DAIKIN/tests/hub/device_names.cpp'),str(build/'WString.cpp'),*objects,
            '-Wl,--gc-sections','-pthread','-o',str(cls.binary)],check=True)

    def test_production_event_cache_and_numbered_diagnostics(self):
        subprocess.run([str(self.binary)],check=True,timeout=30,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
