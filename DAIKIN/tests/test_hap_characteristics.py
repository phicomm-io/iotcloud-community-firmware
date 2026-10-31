"""Run the production HAP characteristic parser and write dispatch on the host."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HapCharacteristics(unittest.TestCase):
    def test_rejected_values_never_reach_service_updates(self):
        source = (ROOT / 'DAIKIN/lib/HomeSpan/src/HomeSpan.cpp').read_text()
        methods = []
        for signature in ('int Span::countCharacteristics(', 'int Span::updateCharacteristics(',
                          'StatusCode SpanCharacteristic::loadUpdate('):
            methods.append(signature + source.split(signature, 1)[1].split(
                '\n///////////////////////////////', 1)[0])
        with tempfile.TemporaryDirectory(prefix='iotcloud-hap-values-') as temp:
            path = Path(temp)
            (path / 'characteristic-methods.inc').write_text('\n'.join(methods))
            binary = path / 'characteristics'
            subprocess.run([
                'c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-fno-pie', '-no-pie', '-I' + str(path),
                '-I' + str(ROOT / 'DAIKIN/lib/HomeSpan/src'),
                str(ROOT / 'DAIKIN/tests/hub/characteristics.cpp'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
