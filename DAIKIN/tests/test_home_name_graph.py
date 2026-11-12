"""Production HomeKit name constructors/callbacks and dynamic synchronization."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from .arduinojson import codec_source
ROOT=Path(__file__).resolve().parents[2]
class HomeNameGraph(unittest.TestCase):
    def test_production_name_callbacks_preserve_paired_identifiers(self):
        source=(ROOT/'DAIKIN/src/home.inc').read_text()
        with tempfile.TemporaryDirectory(prefix='home-name-graph-') as temporary:
            build=Path(temporary)
            (build/'home-name-graph.inc').write_text('static bool homeAwaitControl(){'+source.split('static bool homeAwaitControl(){',1)[1].split('static bool homeMemoryReady(){',1)[0])
            (build/'home-graph-build.inc').write_text('homeAccessory(1,'+source.split('homeAccessory(1,',1)[1].split('if(!homeSpan.iotcloudInitialize())',1)[0])
            binary=build/'test'
            subprocess.run(['c++','-std=c++11','-g','-O1','-Wall','-Wextra','-Werror',
                '-Wno-misleading-indentation','-Wno-sign-compare','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                '-fno-pie','-no-pie','-I'+str(build),'-I'+str(ROOT/'DAIKIN/include'),
                '-I'+str(codec_source()),str(ROOT/'DAIKIN/tests/hub/home_name_graph.cpp'),
                '-lcrypto','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=15,
                           env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
