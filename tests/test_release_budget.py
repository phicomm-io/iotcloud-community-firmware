"""Release budgets fail closed on missing artifacts and boundary overflows."""
import importlib.util
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('esp_budget', Path(__file__).resolve().parents[1] / 'tools/release/check_esp_budget.py')
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


class EspBudgetTests(unittest.TestCase):
    def test_smallest_ota_partition_controls_budget(self):
        with tempfile.TemporaryDirectory() as tmp:
            partitions = Path(tmp) / 'partitions.csv'
            partitions.write_text('# Name, Type, SubType, Offset, Size\na,app,ota_0,0x10000,1920K\nb,app,ota_1,0x1f0000,0x1e0000\n')
            self.assertEqual(budget.ota_capacity(partitions), 1966080)
            partitions.write_text('a,app,ota_0,0x10000,1920K\n')
            with self.assertRaisesRegex(ValueError, 'Two usable OTA'):
                budget.ota_capacity(partitions)

    def measure_fixture(self, image_size=65536, ram=98304, capacity=98304, layout='stock',
                        frame=672, disassembly=None, variant='hub'):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'firmware.bin').write_bytes(bytes(image_size))
            (root / 'firmware.elf').write_bytes(b'unit-test ELF fixture')
            (root / 'partitions.csv').write_text(f'a,app,ota_0,0,{capacity}\nb,app,ota_1,{capacity},{capacity + 32768}\n')
            sections = f'.dram0.data 100 0\n.dram0.bss {ram - 100} 0\n'
            if disassembly is None:
                disassembly = f'400eff68 <_Z4loopv>:\n400eff68:\t054136\tentry\ta1, 0x{frame:x}\n'
            with patch.object(budget.subprocess, 'check_output', side_effect=[sections, disassembly]) as command:
                result = budget.measure(root, root / 'partitions.csv', 'fixture-size', variant, layout)
                self.assertEqual(command.call_count, 2 if variant == 'hub' else 1)
                if variant == 'hub':
                    self.assertEqual(command.call_args.args[0], ['fixture-objdump', '-d', '--disassemble=_Z4loopv', str(root / 'firmware.elf')])
                return result

    def test_flash_reserve_and_static_ram_boundaries(self):
        result = self.measure_fixture()
        self.assertEqual(result['application_bytes'] + result['flash_reserve_bytes'], result['ota_slot_bytes'])
        self.assertEqual(result['static_ram_bytes'], result['static_ram_limit_bytes'])
        self.assertEqual(result['firmware_variant'], 'hub')
        self.assertEqual(result['loop_stack_frame_bytes'], 672)
        for kwargs in ({'image_size': 65537}, {'ram': 98305}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ValueError, 'exceeds'):
                self.measure_fixture(**kwargs)

    def test_missing_elf_and_missing_ram_sections_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'firmware.bin').write_bytes(b'fixture')
            with self.assertRaises(FileNotFoundError):
                budget.measure(root, 'unused.csv', 'unused-size', 'hub')
        with self.assertRaisesRegex(ValueError, 'Missing ESP32 RAM'):
            budget.static_ram('.text 123 0\n')

    def test_hub_loop_frame_boundary_and_compiler_regression(self):
        result = self.measure_fixture(frame=1024)
        self.assertEqual(result['loop_stack_frame_bytes'], result['loop_stack_frame_limit_bytes'])
        for frame in (1040, 1808):
            with self.subTest(frame=frame), self.assertRaises(budget.BudgetError) as error:
                self.measure_fixture(frame=frame)
            self.assertEqual(error.exception.report['loop_stack_frame_bytes'], frame)
            self.assertIn(f'loop stack frame {frame} exceeds budget 1024', str(error.exception))

    def test_loop_frame_accepts_objdump_hex_and_decimal_entry(self):
        for operand in ('0x2a0', '672', '0X2A0'):
            with self.subTest(operand=operand):
                self.assertEqual(budget.loop_stack_frame(
                    f'400eff68 <_Z4loopv>:\n400eff68:\t054136\tentry\ta1, {operand}\n'), 672)

    def test_hub_loop_frame_missing_or_malformed_entry_fails_closed(self):
        for disassembly in (
                '', '400eff68 <other>:\n400eff68: 054136 entry a1, 0x2a0\n',
                '400eff68 <_Z4loopv>:\n',
                '400eff68 <_Z4loopv>:\n400eff68: 054136 entry a1, \n',
                '400eff68 <_Z4loopv>:\n400eff68: 054136 entry a1, 0x2a0garbage\n',
                '400eff68 <_Z4loopv>:\n400eff68: 054136 entry a1, -1\n',
                '400eff68 <_Z4loopv>:\n400eff68: 054136 entry a1, 0\n',
                '400eff68 <_Z4loopv>:\n400eff68: 054136 entry a1, 673\n',
                '400eff68 <_Z4loopv>:\n400eff68: 802d21 l32r a2, 400d0020\n'
                '400eff6b <other>:\n400eff6b: 054136 entry a1, 0x2a0\n'):
            with self.subTest(disassembly=disassembly), self.assertRaises(budget.BudgetError) as error:
                self.measure_fixture(disassembly=disassembly)
            self.assertIsNone(error.exception.report['loop_stack_frame_bytes'])
            self.assertFalse(error.exception.report['passed'])
            self.assertIn('cannot measure loop stack frame', str(error.exception))

    def test_full_budget_does_not_require_objdump(self):
        report = self.measure_fixture(variant='full')
        self.assertTrue(report['passed'])
        self.assertNotIn('loop_stack_frame_bytes', report)

    def test_hub_objdump_failure_preserves_measurements(self):
        for failure in (FileNotFoundError('missing objdump'), subprocess.CalledProcessError(1, 'objdump')):
            with self.subTest(failure=failure), self.assertRaises(budget.BudgetError) as error:
                self.measure_fixture(disassembly=failure)
            self.assertEqual(error.exception.report['application_bytes'], 65536)
            self.assertEqual(error.exception.report['static_ram_bytes'], 98304)
            self.assertIsNone(error.exception.report['loop_stack_frame_bytes'])
            self.assertIn('cannot measure loop stack frame', str(error.exception))

    def test_stock_gate_remains_smaller_than_wired_partition_csv(self):
        result = self.measure_fixture(image_size=1540096, capacity=0x1e0000)
        self.assertEqual(result['ota_layout'], 'stock')
        self.assertEqual(result['ota_slot_bytes'], 0x180000)
        self.assertEqual(result['configured_ota_slot_bytes'], 0x1e0000)
        self.assertTrue(result['stock_ota_compatible'])
        with self.assertRaises(budget.BudgetError) as error:
            self.measure_fixture(image_size=1540097, capacity=0x1e0000)
        self.assertFalse(error.exception.report['stock_ota_compatible'])
        self.assertTrue(error.exception.report['wired_ota_compatible'])
        wired = self.measure_fixture(image_size=1912128, capacity=0x1e0000, layout='wired')
        self.assertTrue(wired['passed'])
        self.assertFalse(wired['stock_ota_compatible'])
        self.assertEqual(wired['ota_slot_bytes'], 0x1e0000)

    def test_cli_preserves_failure_measurements_and_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'firmware.bin').write_bytes(bytes(1540097))
            (root / 'firmware.elf').write_bytes(b'unit-test ELF fixture')
            (root / 'partitions.csv').write_text('a,app,ota_0,0,0x1e0000\nb,app,ota_1,0x1f0000,0x1e0000\n')
            size = root / 'test-size'
            size.write_text('#!' + sys.executable + '\nprint(".dram0.data 100 0\\n.dram0.bss 200 0")\n')
            size.chmod(0o755)
            objdump = root / 'test-objdump'
            objdump.write_text('#!' + sys.executable + '\nprint("400eff68 <_Z4loopv>:\\n400eff68: 054136 entry a1, 0x2a0")\n')
            objdump.chmod(0o755)
            output = root / 'budget.json'
            result = subprocess.run([
                sys.executable, str(Path(budget.__file__)), '--variant', 'hub',
                '--build-dir', str(root), '--partitions', str(root / 'partitions.csv'),
                '--size-tool', str(size), '--output', str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            report = json.loads(output.read_text())
            self.assertFalse(report['passed'])
            self.assertFalse(report['stock_ota_compatible'])
            self.assertTrue(report['wired_ota_compatible'])
            self.assertIn('stock OTA slot', result.stderr)


if __name__ == '__main__':
    unittest.main()
