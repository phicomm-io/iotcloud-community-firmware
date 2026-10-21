#!/usr/bin/env python3
"""Exercise A1/M1 production OTA cores with real build images and mocked Flash.
The real download worker uses an in-memory fetch transport; watchdog timing is tested in test_watchdog.c.
Disposable signing keys never touch the production keyring.
"""
import ctypes
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('package', ROOT / 'tools/ota/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class MicoAdapter(unittest.TestCase):
    def test_adapters(self):
        for model in ('a1', 'm1'):
            with self.subTest(model=model):
                image = Path(os.environ.get(f'{model.upper()}_TEST_IMAGE', ROOT / f'build/{model}-release/phicomm_{model}.ota.bin'))
                if not image.exists():
                    self.skipTest(f'{model} native artifact unavailable; set {model.upper()}_TEST_IMAGE')
                self.check_adapter(model, image.read_bytes())

    def check_adapter(self, model, image):
        key = ec.generate_private_key(ec.SECP256R1())
        public = key.public_key().public_numbers()
        raw = public.x.to_bytes(32, 'big') + public.y.to_bytes(32, 'big')
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'keyring.h').write_text('#include <stdint.h>\n#define OTA_KEY_COUNT 1\nstatic const struct {uint32_t id;uint8_t public_key[64];} ota_keys[]={{1,{' + ','.join(map(str, raw)) + '}}};')
            (d / 'signed_ota.c').write_text((ROOT / 'common/ota/signed_ota.c').read_text())
            (d / 'sha.h').write_text('''#include <openssl/sha.h>
typedef SHA256_CTX SHA256Context;
static int SHA256Reset(SHA256Context *s){return !SHA256_Init(s);}
static int SHA256Input(SHA256Context *s,const uint8_t *p,size_t n){return !SHA256_Update(s,p,n);}
static int SHA256Result(SHA256Context *s,uint8_t *p){return !SHA256_Final(p,s);}
''')
            (d / 'mico.h').write_text('''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include "fetch.h"
typedef uintptr_t mico_thread_arg_t;
static unsigned deleted;
static void mico_rtos_delete_thread(void *t){deleted++;}
#include <openssl/md5.h>
typedef MD5_CTX md5_context;
#define InitMd5 MD5_Init
#define Md5Update MD5_Update
static void Md5Final(md5_context *ctx,uint8_t *out){MD5_Final(out,ctx);}
#define kNoErr 0
#define MICO_PARTITION_OTA_TEMP 1
typedef struct {uint32_t partition_length;} mico_logic_partition_t;
mico_logic_partition_t *MicoFlashGetInfo(int);
int MicoFlashRead(int,uint32_t *,void *,unsigned);
int MicoFlashWrite(int,uint32_t *,uint8_t *,unsigned);
int MicoFlashErase(int,uint32_t,unsigned);
int mico_ota_switch_to_new_fw(int,uint16_t);
static void mico_rtos_lock_mutex(int *m){}
static void mico_rtos_unlock_mutex(int *m){}
static uint32_t mico_rtos_get_time(void){return 1000;}
static int device_lock;
static bool job_active;
int job_releases;
static bool replace_job_on_release;
static bool ota_job_claim(void){if(job_active)return false;job_active=true;return true;}
static bool ota_job_active(void){return job_active;}
static bool ota_job_enter(void){return true;}
static void ota_job_release(void){assert(job_releases++==0);job_active=replace_job_on_release;}
static bool ota_job_cancelled(void){return false;}
static bool ota_job_commit(void){return true;}

static struct {char ota_state[32],ota_error[128],ota_release_counter[21],ota_version[32];
 unsigned ota_progress,ota_key_id,ota_image_length,reboot_at;
 bool ota_active,ota_signature_verified,ota_image_verified,reboot_pending,wifi_trial_active;} device_state;
static bool device_upgrade_reserve(void){if(device_state.ota_active)return false;device_state.ota_active=true;return true;}
''')
            # Keep actual validation, Flash helpers and product constants. Only
            # replace the board state dependency and omit socket transport.
            source = (ROOT / model.upper() / 'ota.c').read_text().split(f'bool {model}_ota_download')[0]
            source = source.replace(f'#include "{model}.h"', '#include "mico.h"')
            source = source.replace('"../common/watchdog_flash.h"', f'"{ROOT}/common/watchdog_flash.h"')
            source = source.replace('"../common/ota/signed_ota.h"', '"signed_ota.h"')
            source = source.replace('#include "../common/ota/fetch.h"', '').replace('#include "../common/ota/mode.h"','')
            source = source.replace(f'{model}_state', 'device_state').replace(f'{model}_lock', 'device_lock').replace(f'{model}_upgrade_reserve', 'device_upgrade_reserve')
            constants = '\n'.join(line for line in (ROOT / model.upper() / f'{model}.h').read_text().splitlines() if re.match(r'#define \w+_(PRODUCT|APP_OFFSET|APP_CPU_ADDRESS|APP_LIMIT) ', line))
            crc = (ROOT / 'A1/protocol.c').read_text().split('uint16_t a1_crc16', 1)[1]
            crc = 'uint16_t ' + model + '_crc16' + crc
            (d / 'adapter.c').write_text('#include "mico.h"\n' + constants + '\n' + crc + '\n' + source)
            harness = '''#include "adapter.c"
static uint8_t flash[0xb5000];
static mico_logic_partition_t part={sizeof flash};
int erases,writes,reads,switches,fault,lease,progress;
bool watchdog_ota_begin(void){assert(!lease);lease=1;return true;}
bool watchdog_ota_progress(void){assert(lease);progress++;return fault!=7;}
void watchdog_ota_end(void){lease=0;}
mico_logic_partition_t *MicoFlashGetInfo(int p){return &part;}
int MicoFlashRead(int p,uint32_t *o,void *d,unsigned n){assert(lease && n<=512);reads++;if(fault==3)return -1;assert(*o+n<=sizeof flash);memcpy(d,flash+*o,n);*o+=n;return 0;}
int MicoFlashWrite(int p,uint32_t *o,uint8_t *d,unsigned n){assert(lease && n<=512);writes++;if(fault==2)return -1;assert(*o+n<=sizeof flash);memcpy(flash+*o,d,n);*o+=n;return 0;}
int MicoFlashErase(int p,uint32_t o,unsigned n){assert(lease && n==4096 && o==(unsigned)erases*4096);erases++;if(fault==1 && erases==2)return -1;memset(flash+o,255,n);return 0;}
int mico_ota_switch_to_new_fw(int n,uint16_t crc){assert(lease);switches++;return fault==5?-1:0;}
static void reset(int failure){commit_uncertain=false;download_running=false;deleted=0;job_active=false;job_releases=0;replace_job_on_release=true;memset(&device_state,0,sizeof device_state);erases=writes=reads=switches=progress=lease=0;fault=failure;}
int start(unsigned n,int failure,int rescue){reset(failure);return rescue?MODEL_ota_begin_unsigned(n):MODEL_ota_begin(n);}
int feed(const uint8_t *p,unsigned n){return MODEL_ota_write(p,n);}
int finish(void){return MODEL_ota_finish();}
void corrupt_flash(void){flash[123]^=1;}
int reboot_pending(void){return device_state.reboot_pending;}
int reboot_time(void){return device_state.reboot_at;}
int uncertain(void){return activation_uncertain();}
int active(void){return job_active && device_state.ota_active;}
int error_visible(void){return !strcmp(device_state.ota_state,"failed") && strstr(device_state.ota_error,"boot metadata uncertain")!=NULL;}
void abort_again(void){MODEL_ota_abort("late transport error");}
int retry(unsigned n){return MODEL_ota_begin(n);}
static const uint8_t *fetch_data;static unsigned fetch_length;
bool ota_fetch(const char *url,uint32_t limit,ota_fetch_begin begin,ota_fetch_write write,void *ctx,const char **error){
 assert(fetch_length<=limit);*error="fetch cleanup diagnostic";
 if(!begin(ctx,fetch_length))return false;
 for(unsigned i=0;i<fetch_length;i+=2048){unsigned n=fetch_length-i>2048?2048:fetch_length-i;if(!write(ctx,i,fetch_data+i,n))return false;}
 return true;
}
int download(const uint8_t *data,unsigned n,int failure){
 reset(failure);assert(reserve());download_running=true;fetch_data=data;fetch_length=n;
 char *url=malloc(2);assert(url);strcpy(url,"x");download_worker((mico_thread_arg_t)url);
 assert(deleted==1 && !download_running);return device_state.reboot_pending;
}
'''.replace('MODEL', model)
            (d / 'harness.c').write_text(harness)
            subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wno-deprecated-declarations', '-I'+str(d), '-I'+str(ROOT/'common/ota'), str(d/'signed_ota.c'), str(ROOT/'common/ota/crypto.c'), str(d/'harness.c'), '-lcrypto', '-o', str(d/'test.so')], check=True)
            lib = ctypes.CDLL(str(d/'test.so'))
            lib.feed.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            lib.download.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int]
            count = lambda name: ctypes.c_int.in_dll(lib, name).value

            def upload(data, failure=0, rescue=False, corrupt=False):
                ok = bool(lib.start(len(data), failure, rescue))
                for offset in range(0, len(data), 2048):
                    if not ok:
                        break
                    chunk = data[offset:offset+2048]
                    ok = bool(lib.feed(chunk, len(chunk)))
                    self.assertEqual(count('lease'), 0)
                if ok:
                    if corrupt:
                        lib.corrupt_flash()
                    ok = bool(lib.finish())
                self.assertEqual(count('lease'), 0)
                self.assertEqual(count('job_releases'), 0 if ok or failure == 5 else 1)
                self.assertEqual(bool(lib.reboot_pending()), ok)
                if ok: self.assertEqual(lib.reboot_time(),2500)
                return ok

            signed = package.sign(image, model, '0.9.4', 'a'*40, key)
            self.assertTrue(upload(signed))
            self.assertEqual(count('switches'), 1)
            self.assertEqual(count('erases'), 0xb5000//4096)
            self.assertGreater(count('progress'), count('erases')+count('writes'))
            self.assertTrue(upload(image, rescue=True))
            for rescue, data in ((False, signed), (True, image)):
                for failure in (1, 2, 3, 7):
                    self.assertFalse(upload(data, failure, rescue))
                    self.assertEqual(count('switches'), 0)
            # Metadata errors may follow a completed primary Flash commit.
            # Late aborts, repeated finish/write and new uploads must not erase
            # the potentially selected image or release either reservation.
            for rescue, data in ((False, signed), (True, image)):
                self.assertFalse(upload(data, 5, rescue))
                self.assertTrue(lib.uncertain())
                self.assertTrue(lib.active())
                self.assertTrue(lib.error_visible())
                before = tuple(count(n) for n in ('erases','writes','reads','switches'))
                lib.abort_again()
                self.assertFalse(lib.finish())
                self.assertFalse(lib.feed(b'x', 1))
                self.assertFalse(lib.retry(len(signed)))
                self.assertTrue(lib.active())
                self.assertTrue(lib.error_visible())
                self.assertEqual(count('job_releases'), 0)
                self.assertEqual(before, tuple(count(n) for n in ('erases','writes','reads','switches')))
                self.assertFalse(lib.reboot_pending())
            # Execute the production download worker's final cleanup too.
            self.assertFalse(lib.download(signed,len(signed),5))
            self.assertTrue(lib.active())
            self.assertTrue(lib.error_visible())
            self.assertEqual(count('job_releases'),0)
            lib.abort_again()
            self.assertTrue(lib.active())
            self.assertTrue(lib.download(signed,len(signed),0))
            self.assertEqual(lib.reboot_time(),2500)
            self.assertEqual(count('job_releases'),0)
            for failure in (1,2,3,7):
                self.assertFalse(lib.download(signed,len(signed),failure))
                self.assertFalse(lib.active())
                self.assertFalse(lib.uncertain())
                self.assertEqual(count('job_releases'),1)
            self.assertFalse(upload(signed, corrupt=True))
            self.assertEqual(count('switches'), 0)
            invalid = bytearray(signed)
            invalid[128] ^= 1
            self.assertFalse(upload(bytes(invalid)))
            self.assertEqual(count('erases'), 0)
            self.assertEqual(count('switches'), 0)
            print(f'{model.upper()}: signed/raw OTA, bounded erase/write/read, lease cleanup, signature/readback/Flash/progress/commit failures passed')


if __name__ == '__main__':
    unittest.main()
