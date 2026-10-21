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
static void mico_thread_msleep(unsigned ms){}
#define MICO_PARTITION_OTA_TEMP 1
#define MICO_PARTITION_BOOTLOADER 2
#define MICO_PARTITION_APPLICATION 3
#define kNoErr 0
typedef struct {uint32_t partition_start_addr,partition_length;} mico_logic_partition_t;
mico_logic_partition_t *MicoFlashGetInfo(int);
int MicoFlashRead(int,uint32_t *,void *,unsigned);
int MicoFlashWrite(int,uint32_t *,uint8_t *,unsigned);
int MicoFlashErase(int,uint32_t,unsigned);
int mico_ota_switch_to_new_fw(int,uint16_t);
''')
  adapter=(ROOT/'TC1/ota_server/signed_adapter.c').read_text().replace('../../common/ota/signed_ota.h','signed_ota.h').replace('#include "../../common/ota/mode.h"','bool ota_job_cancelled(void); bool ota_job_commit(void);')
  (d/'adapter.c').write_text(adapter)
  (d/'harness.c').write_text('''#include "adapter.c"
static uint8_t flash[0xa9000];
static mico_logic_partition_t part={0xaf000,0xa9000},current={0x6000,0xa9000};
int erases,writes,switches,fault;
bool ota_job_cancelled(void){return fault==7 && erases>=2;}
bool ota_job_commit(void){return fault!=8 && !ota_job_cancelled();}
mico_logic_partition_t *MicoFlashGetInfo(int p){if(p==3)return &current;return &part;}
int MicoFlashRead(int p,uint32_t *o,void *d,unsigned n){if(fault==3 || p==2)return -1;if(*o>sizeof flash || n>sizeof flash-*o)return -1;memcpy(d,flash+*o,n);*o+=n;return 0;}
int MicoFlashWrite(int p,uint32_t *o,uint8_t *d,unsigned n){writes++;if(fault==2)return -1;if(*o>sizeof flash || n>sizeof flash-*o)return -1;memcpy(flash+*o,d,n);*o+=n;return 0;}
int MicoFlashErase(int p,uint32_t o,unsigned n){erases++;if(fault==1)return -1;memset(flash,255,sizeof flash);return 0;}
int mico_ota_switch_to_new_fw(int n,uint16_t crc){switches++;return fault==5?-1:0;}
int start(unsigned n,unsigned failure,int rescue){active=commit_uncertain=false;ota_state="idle";erases=writes=switches=0;fault=failure;current.partition_start_addr=fault==6?0xaf000:0x6000;tc1_status_guard_init();tc1_ota_init();return tc1_ota_reserve() && tc1_ota_begin(n,rescue);}
int feed(const uint8_t *p,unsigned n){return tc1_ota_write(p,n);}
int finish(void){return tc1_ota_finish();}
int verify(void){return tc1_ota_verify();}
int activate(void){return tc1_ota_activate();}
int uncertain(void){return tc1_ota_commit_uncertain();}
void lose_response(void){tc1_ota_failure("HTTP response not delivered; upload not activated");}
int config_available(void){bool ok=tc1_config_reserve();if(ok)tc1_config_release();return ok;}
int retry(unsigned n,int rescue){return tc1_ota_reserve() && tc1_ota_begin(n,rescue);}
void cancel_before_activation(void){fault=8;}

void corrupt_flash(void){flash[123]^=1;}
int reserve_again(void){tc1_ota_abort();return tc1_ota_reserve();}
''')
  subprocess.run(['cc','-DPHICOMM_DEVICE_OTA=1','-shared','-fPIC','-O2','-Wno-deprecated-declarations','-I'+str(d),'-I'+str(ROOT/'common/ota'),'-I'+str(ROOT/'TC1/ota_server'),str(d/'signed_ota.c'),str(ROOT/'common/ota/crypto.c'),str(d/'harness.c'),str(ROOT/'TC1/status_guard.c'),'-lcrypto','-o',str(d/'test.so')],check=True)
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
 def test_verified_response_loss_has_no_activation_and_allows_retry(self):
  for rescue in [False,True]:
   data=self.image if rescue else self.signed(self.image)
   self.assertTrue(self.lib.start(len(data),0,rescue))
   self.assertFalse(self.lib.activate());self.assertEqual(self.count('switches'),0)
   for at in range(0,len(data),512):self.assertTrue(self.lib.feed(data[at:at+512],len(data[at:at+512])))
   self.assertTrue(self.lib.verify());self.assertEqual(self.count('switches'),0)
   self.assertFalse(self.lib.config_available())
   self.lib.lose_response();self.assertTrue(self.lib.config_available())
   self.assertTrue(self.lib.retry(len(data),rescue))
   for at in range(0,len(data),512):self.assertTrue(self.lib.feed(data[at:at+512],len(data[at:at+512])))
   self.assertTrue(self.lib.finish());self.assertEqual(self.count('switches'),1)
   self.assertFalse(self.lib.config_available())
 def test_activation_failure_and_cancel_after_verification(self):
  data=self.signed(self.image)
  for failure in [5,8]:
   self.assertTrue(self.lib.start(len(data),failure==5 and 5 or 0,False))
   for at in range(0,len(data),512):self.assertTrue(self.lib.feed(data[at:at+512],len(data[at:at+512])))
   self.assertTrue(self.lib.verify());self.assertEqual(self.count('switches'),0)
   if failure==8:self.lib.cancel_before_activation()
   self.assertFalse(self.lib.activate())
   self.assertEqual(bool(self.lib.uncertain()),failure==5)
   if failure==5:
    self.lib.lose_response();self.assertFalse(self.lib.config_available());self.assertFalse(self.lib.retry(len(data),False))
    self.assertFalse(self.lib.activate());self.assertEqual(self.count('switches'),1)
   else:self.assertTrue(self.lib.config_available());self.assertEqual(self.count('switches'),0)
 def test_valid_signed_and_unsigned_native(self):
  self.assertTrue(self.upload(self.signed(self.image)));self.assertEqual(self.count('switches'),1)
  self.assertFalse(self.lib.verify());self.assertFalse(self.lib.activate());self.assertEqual(self.count('switches'),1)
  self.assertFalse(self.lib.reserve_again())
  self.assertTrue(self.upload(self.image,rescue=True));self.assertEqual(self.count('switches'),1)
 def test_raw_compatibility_skips_new_image_and_flash_read_checks(self):
  data=bytearray(self.image);data[0]^=1;data[-1]^=1
  self.assertTrue(self.upload(bytes(data),rescue=True))
  self.assertEqual(self.count('switches'),1)
  self.assertTrue(self.upload(self.image,fault=3,rescue=True))
  self.assertEqual(self.count('switches'),1)
 def test_raw_transport_failures_do_not_activate(self):
  for fault in [1,2]:
   self.assertFalse(self.upload(self.image,fault,rescue=True))
   self.assertEqual(self.count('switches'),0)
  self.assertTrue(self.lib.start(len(self.image),0,True))
  self.assertTrue(self.lib.feed(self.image[:512],512))
  self.assertFalse(self.lib.finish());self.assertEqual(self.count('switches'),0)
  self.assertFalse(self.lib.start(0,0,True))
  self.assertFalse(self.lib.start(0xa9001,0,True))
  self.assertFalse(self.upload(self.image,fault=5,rescue=True))
  self.assertFalse(self.lib.reserve_again())
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
 def test_commit_error_inhibits_retry(self):
  for fault in [5]:
   self.assertFalse(self.upload(self.signed(self.image),fault));self.assertEqual(self.count('switches'),1)
   self.assertFalse(self.lib.reserve_again())
 def test_active_slot_cannot_be_update_target(self):
  self.assertFalse(self.upload(self.signed(self.image),6));self.assertEqual(self.count('switches'),0)
 def test_cancel_during_erase_and_before_commit(self):
  for rescue in [False,True]:
   data=self.image if rescue else self.signed(self.image)
   self.assertFalse(self.upload(data,7,rescue));self.assertEqual(self.count('switches'),0)
   self.assertLess(self.count('erases'),3)
   self.assertFalse(self.upload(data,8,rescue));self.assertEqual(self.count('switches'),0)
if __name__=='__main__':unittest.main()
