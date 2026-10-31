"""Production hub S30-V1B model and graph contract under ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from DAIKIN.tests.arduinojson import codec_source

ROOT = Path(__file__).resolve().parents[1]


class HubS30(unittest.TestCase):
    def test_discovery_registration_state_graph_and_relay(self):
        source = (ROOT / 'src/home.inc').read_text()
        graph = 'using Home=iotcloud::HubHome;' + source.split(
            'using Home=iotcloud::HubHome;', 1)[1].split(
                'if(!homeSpan.iotcloudInitialize())', 1)[0]
        with tempfile.TemporaryDirectory(prefix='hub-s30-') as temporary:
            build = Path(temporary)
            (build / 'home-device-graph.inc').write_text(graph)
            binary = build / 's30'
            subprocess.run([
                'c++', '-std=c++11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                '-I' + str(ROOT / 'include'), '-I' + str(codec_source()), '-I' + str(build),
                str(ROOT / 'tests/hub/s30.cpp'), '-lcrypto', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                                'UBSAN_OPTIONS': 'halt_on_error=1'})


if __name__ == '__main__':
    unittest.main()
