"""Compare production HAP/TLV output bytes, frame boundaries and SHA-384 on the host."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOME = ROOT / 'DAIKIN/lib/HomeSpan/src'


class HapOutput(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='iotcloud-hap-output-')
        root = Path(cls.tmp.name)
        header = (HOME / 'HAP.h').read_text()
        output_class = 'class HapOut :' + header.split('class HapOut :', 1)[1].split(
            '// Extern Variables', 1)[0]
        nonce_class = 'struct Nonce {' + header.split('struct Nonce {', 1)[1].split(
            '// Accessory Structure', 1)[0]
        source = (HOME / 'HAP.cpp').read_text()
        methods = 'Nonce::Nonce()' + source.split('Nonce::Nonce()', 1)[1].split(
            '// instantiate all static HAP Client structures', 1)[0]
        (root / 'hap-class.inc').write_text(output_class)
        (root / 'nonce-class.inc').write_text(nonce_class)
        (root / 'hap-methods.inc').write_text(methods)
        source = (HOME / 'TLV8.cpp').read_text()
        methods = 'tlv8_t::tlv8_t(' + source.split('tlv8_t::tlv8_t(', 1)[1].split(
            'int TLV8::unpack(', 1)[0]
        methods += 'void TLV8::osprint(' + source.split('void TLV8::osprint(', 1)[1]
        (root / 'tlv-methods.inc').write_text(methods)
        (root / 'Arduino.h').write_text('''#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
using String = std::string;
struct HostSerial {
  template<class... T> void printf(const char *, T...) {}
  void print(const char *) {}
};
extern HostSerial Serial;
''')
        cls.binary = root / 'hap-output'
        subprocess.run([
            'c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
            '-Wno-sign-compare', '-Wno-deprecated-declarations',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-fno-pie', '-no-pie', '-I' + str(root), '-I' + str(HOME),
            '-I' + str(ROOT / 'DAIKIN/include'),
            str(ROOT / 'DAIKIN/tests/hub/hap_output.cpp'), '-lcrypto',
            '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scenario(self, name):
        subprocess.run([str(self.binary), name], check=True, timeout=15,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})

    def test_decimal_json_matches_existing_stream_bytes(self):
        self.scenario('numbers')

    def test_plaintext_and_aead_records_preserve_all_payload_bytes(self):
        self.scenario('frames')

    def test_tlv_empty_binary_and_fragmented_records(self):
        self.scenario('tlv')

    def test_callback_count_flush_and_hash_restart(self):
        self.scenario('callback')

    def test_short_writes_encryption_failure_and_nonce_exhaustion_close(self):
        self.scenario('failures')


if __name__ == '__main__':
    unittest.main()
