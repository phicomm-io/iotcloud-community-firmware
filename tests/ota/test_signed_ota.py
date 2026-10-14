#!/usr/bin/env python3
"""Cross-language vectors and staging failure tests; never uses production keys."""
import ctypes
import os
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ec
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('package', ROOT / 'tools/ota/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

class SignedOTA(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        cls.key = ec.generate_private_key(ec.SECP256R1())
        public = cls.key.public_key().public_numbers()
        raw = public.x.to_bytes(32, 'big') + public.y.to_bytes(32, 'big')
        (d/'keyring.h').write_text('#include <stdint.h>\n#define OTA_KEY_COUNT 1\nstatic const struct {uint32_t id; uint8_t public_key[64];} ota_keys[]={ {1, {' + ','.join(str(x) for x in raw) + '}} };')
        (d/'signed_ota.c').write_text((ROOT/'common/ota/signed_ota.c').read_text())
        (d/'sha.h').write_text('''#include <openssl/sha.h>
typedef SHA256_CTX SHA256Context;
static int SHA256Reset(SHA256Context *s){return !SHA256_Init(s);}
static int SHA256Input(SHA256Context *s,const uint8_t *p,size_t n){return !SHA256_Update(s,p,n);}
static int SHA256Result(SHA256Context *s,uint8_t *p){return !SHA256_Final(p,s);}
''')
        (d/'harness.c').write_text('''#include "signed_ota.h"
#include <string.h>
static signed_ota s; static uint8_t flash[8192];
int erases,writes,reads,fault;
static bool erase(void *c){erases++;memset(flash,255,sizeof flash);return fault!=1;}
static bool writef(void *c,uint32_t o,const uint8_t *p,size_t n){writes++;if(fault==2)return false;memcpy(flash+o,p,n);return true;}
static bool readf(void *c,uint32_t o,uint8_t *p,size_t n){reads++;if(fault==3)return false;memcpy(p,flash+o,n);if(fault==4)p[0]^=1;return true;}
int begin(unsigned n,unsigned model,unsigned failure){erases=writes=reads=0;fault=failure;signed_ota_io io={erase,writef,readf,0};return signed_ota_begin(&s,n,sizeof flash,model,1,io);}
int feed(const uint8_t *p,unsigned n){return signed_ota_write(&s,p,n);}
int finish(void){return signed_ota_finish(&s);}
void counter_decimal(uint64_t value,char *out){ota_counter_decimal(value,out);}
''')
        sha_dir=os.environ.get('OTA_SHA_DIR')
        extra=[]
        if sha_dir:
            (d/'sha.h').unlink()
            extra=['-I'+sha_dir,str(Path(sha_dir)/'sha224-256.c')]
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wno-deprecated-declarations',
                        '-I'+str(d), '-I'+str(ROOT/'common/ota'),
                        str(d/'signed_ota.c'), str(ROOT/'common/ota/crypto.c'),
                        str(d/'harness.c'), *extra, '-lcrypto', '-o', str(d/'test.so')], check=True)
        cls.lib = ctypes.CDLL(str(d/'test.so'))
        cls.lib.feed.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        cls.image = bytes(range(256))*17
        cls.valid = package.sign(cls.image, 'tc1', 'v2.3.5', 'a'*40, cls.key)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_package(self, data, chunk=512, model=1, fault=0):
        if not self.lib.begin(len(data), model, fault):
            return False
        for off in range(0, len(data), chunk):
            if not self.lib.feed(data[off:off+chunk], min(chunk, len(data)-off)):
                return False
        return bool(self.lib.finish())

    def count(self, name):
        return ctypes.c_int.in_dll(self.lib, name).value

    def test_cross_language_and_all_header_boundaries(self):
        for chunk in [1, 7, 127, 128, 191, 192, 193, 512, 8192]:
            self.assertTrue(self.run_package(self.valid, chunk))
            self.assertEqual(self.count('erases'), 1)
            self.assertGreater(self.count('reads'), 0)

    def test_every_header_and_signature_byte_tampering_before_erase(self):
        for offset in range(192):
            mutated = bytearray(self.valid); mutated[offset] ^= 1
            self.assertFalse(self.run_package(bytes(mutated)))
            self.assertEqual(self.count('erases'), 0, offset)

    def test_wrong_model_and_key(self):
        self.assertFalse(self.run_package(self.valid, model=2))
        self.assertEqual(self.count('erases'), 0)
        bad = package.sign(self.image, 'tc1', '2.3.5', 'a'*40, ec.generate_private_key(ec.SECP256R1()))
        self.assertFalse(self.run_package(bad))
        self.assertEqual(self.count('erases'), 0)

    def test_image_flash_faults_and_length(self):
        mutated = bytearray(self.valid); mutated[-1] ^= 1
        self.assertFalse(self.run_package(bytes(mutated)))
        for fault in range(1, 5):
            self.assertFalse(self.run_package(self.valid, fault=fault))
        for length in [0, 1, 128, 191, 192, len(self.valid)-1]:
            self.assertFalse(self.run_package(self.valid[:length]))
        self.assertFalse(self.run_package(self.valid+b'x'))
        self.assertEqual(self.count('erases'), 0)
        self.assertFalse(self.lib.begin(0xffffffff, 1, 0))

    def test_interrupted_and_failed_session_cannot_finish(self):
        self.assertTrue(self.lib.begin(len(self.valid), 1, 0))
        self.assertTrue(self.lib.feed(self.valid[:1000], 1000))
        self.assertFalse(self.lib.finish())
        self.assertFalse(self.lib.feed(self.valid[1000:], len(self.valid)-1000))
        self.assertFalse(self.lib.finish())

    def test_counter_decimal_full_uint64(self):
        self.lib.counter_decimal.argtypes=[ctypes.c_uint64,ctypes.c_void_p]
        for value in [0, 1, 2**32, 2**53+1, 2**64-1]:
            out=ctypes.create_string_buffer(21)
            self.lib.counter_decimal(value,out)
            self.assertEqual(out.value.decode(),str(value))

    def test_reinstall_and_downgrade_allowed(self):
        for version in ['2.3.5', '2.3.5', '1.0.0']:
            self.assertTrue(self.run_package(package.sign(self.image, 'tc1', version, 'a'*40, self.key)))

if __name__ == '__main__':
    unittest.main()
