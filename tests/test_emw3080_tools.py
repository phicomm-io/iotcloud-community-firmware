"""Offline checks: reject wrong/corrupt images before invoking a programmer."""
import argparse
import binascii
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('emw3080', ROOT / 'docs/public/assets/tools/emw3080-openocd/emw3080.py')
tool = importlib.util.module_from_spec(SPEC)
previous_bytecode = sys.dont_write_bytecode
try:
    sys.dont_write_bytecode = True  # The imported helper is also a public asset.
    SPEC.loader.exec_module(tool)
finally:
    sys.dont_write_bytecode = previous_bytecode


class FlashPreparationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.image = self.root / 'phicomm_m1.all.bin'
        self.checksums = self.root / 'SHA256SUMS'
        self.data = bytearray(b'\xff' * 0x110000)
        body = struct.pack('<III', 0xC89346, 0x10010000, 3) + b'PHICOMM_M1_EMW3080\0' + bytes(100)
        crc = binascii.crc_hqx(body, 0)
        app = struct.pack('<IHH', len(body), crc, crc) + body
        self.end = 0x88000 + len(app)
        self.data[0x88000:self.end] = app
        self.data[self.end:self.end+16] = hashlib.md5(self.data[0x13000:self.end]).digest()
        self.save()

    def save(self):
        self.image.write_bytes(self.data)
        self.checksums.write_text(f'{tool.sha256(self.data)}  {self.image.name}\n')

    def split(self):
        return tool.split_image(self.image, 'm1', self.checksums)

    def test_exact_legacy_ranges_and_no_parameter_writes(self):
        parts = self.split()
        self.assertEqual([len(v) for v in parts.values()], [72*1024, 724*1024, 256*1024])
        self.assertEqual(parts['boot.bin'], self.data[:0x12000])
        self.assertEqual(parts['ota.bin'], self.data[0x13000:0xC8000])
        self.assertEqual(parts['ate.bin'], self.data[0xD0000:])

    def test_mismatched_release_checksum(self):
        self.image.write_bytes(bytes(len(self.data)))
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            self.split()

    def test_wrong_product_even_after_renaming(self):
        image = self.root / 'phicomm_a1.all.bin'
        image.write_bytes(self.data)
        self.checksums.write_text(f'{tool.sha256(self.data)}  {image.name}\n')
        with self.assertRaisesRegex(ValueError, 'product mismatch'):
            tool.split_image(image, 'a1', self.checksums)

    def test_nonempty_parameter_data_rejected(self):
        self.data[0xC8000] = 0
        self.save()
        with self.assertRaisesRegex(ValueError, 'parameter data'):
            self.split()

    def test_corrupt_application_rejected_despite_updated_sha256(self):
        self.data[0x88030] ^= 1
        self.save()
        with self.assertRaisesRegex(ValueError, 'CRC'):
            self.split()

    def test_corrupt_kernel_rejected_despite_updated_sha256(self):
        self.data[0x14000] ^= 1
        self.save()
        with self.assertRaisesRegex(ValueError, 'MD5'):
            self.split()

    def test_truncated_image_rejected(self):
        self.data = self.data[:0xD0000]
        self.save()
        with self.assertRaisesRegex(ValueError, 'length'):
            self.split()

    def test_existing_output_not_overwritten(self):
        with self.assertRaises(FileExistsError):
            tool.write_parts(self.split(), self.root)
        self.assertEqual(self.image.read_bytes(), self.data)

    def test_unknown_programmer_rejected_before_execution(self):
        (self.root / 'sflash_write.tcl').write_bytes(b'wrong script')
        with self.assertRaisesRegex(ValueError, 'Tool package mismatch'):
            tool.openocd_command(argparse.Namespace(engine='modern', openocd=None), self.root)

    def test_nonzero_openocd_exit_propagates(self):
        with patch.object(tool.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, ['openocd'])) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                tool.run_openocd(['openocd'], self.root)
        run.assert_called_once_with(['openocd'], cwd=self.root, check=True)


if __name__ == '__main__':
    unittest.main()
