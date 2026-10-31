"""Keep the vendored input formats within the ESP32 integer scanner contract."""
from pathlib import Path
import ast
import re
import unittest

HOME = Path(__file__).resolve().parents[1] / 'lib/HomeSpan/src'


class HapScan(unittest.TestCase):
    def test_vendored_input_formats_need_no_float_or_dynamic_format(self):
        calls = 0
        conversion = re.compile(r'%(?:\*?\d*)(?:hh|ll|h|l|j|z|t)?(?:\[[^\]]+\]|[diouxXcspn%])')
        for path in sorted(HOME.rglob('*')):
            if path.suffix not in ('.cpp', '.h', '.ino') or path.name == 'IoTCloudScan.h':
                continue
            text = path.read_text()
            self.assertIsNone(re.search(r'\b(?:v?sscanf|siscanf)\s*\(', text), path)
            for call in re.finditer(r'\bIOTCLOUD_HAP_SCAN\s*\(', text):
                # A nonliteral format must be reviewed before changing scanner.
                literal = re.match(r'IOTCLOUD_HAP_SCAN\s*\([^,\n]*,\s*("(?:\\.|[^"\\])*")\s*,', text[call.start():])
                self.assertIsNotNone(literal, (path, text[call.start():call.start() + 120]))
                fmt = ast.literal_eval(literal.group(1))
                at = 0
                while '%' in fmt[at:]:
                    at = fmt.index('%', at)
                    parsed = conversion.match(fmt, at)
                    self.assertIsNotNone(parsed, (path, fmt))
                    at = parsed.end()
                calls += 1
        self.assertGreater(calls, 0)


if __name__ == '__main__':
    unittest.main()
