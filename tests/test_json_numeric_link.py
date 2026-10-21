"""A linked native scanner is insufficient evidence for the checked JSON path."""
import unittest
from tools.check_json_float import REQUIRED, check


class NumericLink(unittest.TestCase):
    def test_private_component_is_required(self):
        symbols = '\n'.join('08000000 T '+name for name in REQUIRED)
        check(symbols)
        with self.assertRaisesRegex(ValueError, 'missing or duplicated'):
            check('08000000 T _scanf_float\n')
        for name in REQUIRED:
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                check('\n'.join(line for line in symbols.splitlines() if not line.endswith(' '+name)))
        with self.assertRaisesRegex(ValueError, 'json_number_scan'):
            check(symbols+'\n08000004 T json_number_scan\n')
