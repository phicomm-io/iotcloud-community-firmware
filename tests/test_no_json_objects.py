"""Exercise the dependency gate against real host-linked executables."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('no_json', Path(__file__).resolve().parents[1] / 'tools/check_no_json_objects.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


@unittest.skipUnless(shutil.which('cc') and shutil.which('nm'), 'host compiler and nm required')
class LinkedDependencyGate(unittest.TestCase):
    def test_real_linked_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for symbol, rejected in [('json_object_new_object', True),
                                     ('json_tokener_parse_ex', True),
                                     ('json_num_strtod', True),
                                     ('ha_json_write_string', False),
                                     ('phw_map', False)]:
                source, elf = root / 'fixture.c', root / 'fixture'
                source.write_text(f'int {symbol}(void) {{ return 1; }}\nint main(void) {{ return {symbol}(); }}\n')
                subprocess.run(['cc', '-O0', str(source), '-o', str(elf)], check=True)
                self.assertEqual(bool(gate.inspect(elf, 'nm')), rejected, symbol)
