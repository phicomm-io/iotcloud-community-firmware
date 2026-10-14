#!/usr/bin/env python3
"""Run the actual TC1 adapter against fault-injected Flash and boot metadata.
Uses a disposable test keyring, never the production private key.
"""
import ctypes
import hashlib
import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ec
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('package',ROOT/'tools/ota/package.py')
package=importlib.util.module_from_spec(spec);spec.loader.exec_module(package)
IMAGE=Path(os.environ.get('TC1_TEST_IMAGE',ROOT/'build/tc1-release/phicomm_tc1_a1.ota.bin'))

class TC1Adapter(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if not IMAGE.exists():raise unittest.SkipTest('TC1 native artifact unavailable; set TC1_TEST_IMAGE')
  cls.image=IMAGE.read_bytes();cls.key=ec.generate_private_key(ec.SECP256R1())
  cls.tmp=tempfile.TemporaryDirectory();d=Path(cls.tmp.name)
  public=cls.key.public_key().public_numbers();raw=public.x.to_bytes(32,'big')+public.y.to_bytes(32,'big')
  (d/'keyring.h').write_text('#include <stdint.h>\n#define OTA_KEY_COUNT 1\nstatic const struct {uint32_t id;uint8_t public_key[64];} ota_keys[]={{1,{'+','.join(str(x) for x in raw)+'}}};')
  (d/'signed_ota.c').write_text((ROOT/'common/ota/signed_ota.c').read_text())
  (d/'sha.h').write_text('''#include <openssl/sha.h>
typedef SHA256_CTX SHA256Context;
static int SHA256Reset(SHA256Context *s){return !SHA256_Init(s);}
static int SHA256Input(SHA256Context *s,const uint8_t *p,size_t n){return !SHA256_Update(s,p,n);}
static int SHA256Result(SHA256Context *s,uint8_t *p){return !SHA256_Final(p,s);}
''')
  (d/'mico.h').write_text('''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <openssl/md5.h>
typedef MD5_CTX md5_context;
#define InitMd5 MD5_Init
#define Md5Update MD5_Update
static void Md5Final(md5_context *ctx,uint8_t *out){MD5_Final(out,ctx);}
typedef int mico_mutex_t;
static int mico_rtos_init_mutex(mico_mutex_t *m){return 0;}
static void mico_rtos_lock_mutex(mico_mutex_t *m){}
static void mico_rtos_unlock_mutex(mico_mutex_t *m){}
#define MICO_PARTITION_OTA_TEMP 1
#define MICO_PARTITION_BOOTLOADER 2
#define kNoErr 0
typedef struct {uint32_t partition_start_addr,partition_length;} mico_logic_partition_t;
mico_logic_partition_t *MicoFlashGetInfo(int);
int MicoFlashRead(int,uint32_t *,void *,unsigned);
int MicoFlashWrite(int,uint32_t *,uint8_t *,unsigned);
int MicoFlashErase(int,uint32_t,unsigned);
int mico_ota_switch_to_new_fw(int,uint16_t);
''')
  adapter=(ROOT/'TC1/ota_server/signed_adapter.c').read_text().replace('../../common/ota/signed_ota.h','signed_ota.h')
  (d/'adapter.c').write_text(adapter)
  (d/'harness.c').write_text('''#include "adapter.c"
static uint8_t flash[0xa9000],boot[0x6000];
static mico_logic_partition_t part={0xaf000,0xa9000};
int erases,writes,switches,fault;
mico_logic_partition_t *MicoFlashGetInfo(int p){return &part;}
int MicoFlashRead(int p,uint32_t *o,void *d,unsigned n){if(fault==3)return -1;unsigned size=p==2?sizeof boot:sizeof flash;if(*o>size || n>size-*o)return -1;memcpy(d,(p==2?boot:flash)+*o,n);*o+=n;return 0;}
int MicoFlashWrite(int p,uint32_t *o,uint8_t *d,unsigned n){writes++;if(fault==2)return -1;if(*o>sizeof flash || n>sizeof flash-*o)return -1;memcpy(flash+*o,d,n);*o+=n;return 0;}
int MicoFlashErase(int p,uint32_t o,unsigned n){erases++;if(fault==1)return -1;memset(flash,255,sizeof flash);return 0;}
static void put32(uint8_t *p,uint32_t n){p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static void table(unsigned off,unsigned generation,bool new_slot){uint8_t *h=boot+off;memset(h,0,68);memcpy(h,"WMPT",4);h[4]=1;h[6]=2;put32(h+8,generation);put32(h+12,table_crc(h,12));for(unsigned i=0;i<2;i++){uint8_t *e=h+16+i*24;e[0]=1;put32(e+12,i?0xaf000:0x6000);put32(e+16,0xa9000);put32(e+20,(i==new_slot)?1:0);}put32(h+64,table_crc(h+16,48));}
int mico_ota_switch_to_new_fw(int n,uint16_t crc){switches++;if(fault==5)return -1;if(fault!=6)table(0x5000,1,true);return 0;}
int start(unsigned n,unsigned failure,int rescue){active=commit_uncertain=false;ota_state="idle";erases=writes=switches=0;fault=failure;table(0x4000,0,false);table(0x5000,0,false);tc1_ota_init();return tc1_ota_reserve() && tc1_ota_begin(n,rescue);}
int feed(const uint8_t *p,unsigned n){return tc1_ota_write(p,n);}
int finish(void){return tc1_ota_finish();}
void corrupt_flash(void){flash[123]^=1;}
int reserve_again(void){tc1_ota_abort();return tc1_ota_reserve();}
''')
  subprocess.run(['cc','-shared','-fPIC','-O2','-Wno-deprecated-declarations','-I'+str(d),'-I'+str(ROOT/'common/ota'),'-I'+str(ROOT/'TC1/ota_server'),str(d/'signed_ota.c'),str(ROOT/'common/ota/crypto.c'),str(d/'harness.c'),'-lcrypto','-o',str(d/'test.so')],check=True)
  cls.lib=ctypes.CDLL(str(d/'test.so'));cls.lib.feed.argtypes=[ctypes.c_void_p,ctypes.c_uint]
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def signed(self,image):return package.sign(image,'tc1','0.9.2','a'*40,self.key)
 def upload(self,data,fault=0,rescue=False):
  if not self.lib.start(len(data),fault,rescue):return False
  for offset in range(0,len(data),512):
   part=data[offset:offset+512]
   if not self.lib.feed(part,len(part)):return False
  return bool(self.lib.finish())
 def count(self,name):return ctypes.c_int.in_dll(self.lib,name).value
 def test_valid_signed_and_unsigned_native(self):
  self.assertTrue(self.upload(self.signed(self.image)));self.assertEqual(self.count('switches'),1)
  self.assertFalse(self.lib.reserve_again())
  self.assertTrue(self.upload(self.image,rescue=True));self.assertEqual(self.count('switches'),1)
 def test_no_activation_after_invalid_signature_or_body(self):
  data=bytearray(self.signed(self.image));data[128]^=1
  self.assertFalse(self.upload(bytes(data)));self.assertEqual(self.count('erases'),0);self.assertEqual(self.count('switches'),0)
  data=bytearray(self.signed(self.image));data[-1]^=1
  self.assertFalse(self.upload(bytes(data)));self.assertEqual(self.count('switches'),0)
  self.assertFalse(self.upload(self.image));self.assertEqual(self.count('erases'),0)
 def test_authenticated_but_incompatible_native(self):
  for offset in [0,0x64000+4,0x64000+16,-1]:
   data=bytearray(self.image);data[offset]^=1
   self.assertFalse(self.upload(self.signed(bytes(data))));self.assertEqual(self.count('switches'),0)
 def test_flash_failures_and_incomplete(self):
  for fault in [1,2,3]:
   self.assertFalse(self.upload(self.signed(self.image),fault));self.assertEqual(self.count('switches'),0)
  data=self.signed(self.image);self.assertTrue(self.lib.start(len(data),0,False))
  self.assertTrue(self.lib.feed(data[:1000],1000));self.assertFalse(self.lib.finish());self.assertEqual(self.count('switches'),0)
 def test_commit_error_and_false_success_inhibit_retry(self):
  for fault in [5,6]:
   self.assertFalse(self.upload(self.signed(self.image),fault));self.assertEqual(self.count('switches'),1)
   self.assertFalse(self.lib.reserve_again())
if __name__=='__main__':unittest.main()
