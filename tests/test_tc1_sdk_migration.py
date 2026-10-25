"""Run the rebased SDK's old-parameter reader and new Flash writer together."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


class SDKMigration(unittest.TestCase):
    def test_old_current_and_frozen_layouts_flash_faults_and_new_crc(self):
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
        path = sdk / 'MiCO/system/mico_system_para_storage.c'
        if not path.exists():
            self.skipTest('build TC1 first')
        source = path.read_text()
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int OSStatus;
typedef int mico_partition_t;
typedef int mico_mutex_t;
typedef struct { uint16_t value; } CRC16_Context;
typedef struct { uint32_t partition_length; } mico_logic_partition_t;
typedef struct { uint8_t boot[24]; struct { uint32_t magic_number; int32_t seed; } micoSystemConfig; } system_config_t;
typedef struct { system_config_t flashContentInRam; void *user_config_data; uint32_t user_config_data_size; } system_context_t;
#define kNoErr 0
#define kNotPreparedErr -1
#define kWriteErr -2
#define kSizeErr -3
#define kReadErr -4
#define kNoMemoryErr -5
#define kParamErr -6
#define MICO_PARTITION_PARAMETER_1 0
#define MICO_PARTITION_PARAMETER_2 1
#define PARA_MICO_DATA_SECTION 0
#define PARA_APP_DATA_SECTION 1
#define CRC_SIZE 2
#define SYS_CONFIG_SIZE 8
#define SYS_MAGIC_NUMBR 0xA43E2165
#define require_action(c,l,a) do { if (!(c)) { a; goto l; } } while(0)
#define require_noerr(e,l) do { if ((e)!=0) goto l; } while(0)
#define para_log(...) ((void)0)
static uint8_t flash[2][256], data[64];
static uint32_t mico_context_section_offsets[] = {24, 32};
static system_context_t context;
static int seedNum, writes, restores, migrations, read_fault, write_fault;
static mico_mutex_t para_flash_mutex;
static void mico_rtos_lock_mutex(mico_mutex_t *m) { (void)m; }
static void mico_rtos_unlock_mutex(mico_mutex_t *m) { (void)m; }
static void CRC16_Init(CRC16_Context *c) { c->value=0; }
static void CRC16_Update(CRC16_Context *c,const void *bytes,unsigned n) {
 const uint8_t *p=bytes;
 while(n--) { c->value ^= (uint16_t)*p++ << 8;
  for(unsigned i=0;i<8;i++)c->value=(c->value&0x8000)?(c->value<<1)^0x1021:c->value<<1;
 }
}
static void CRC16_Final(CRC16_Context *c,uint16_t *value) { *value=c->value; }
static mico_logic_partition_t *MicoFlashGetInfo(int part) { (void)part; static mico_logic_partition_t p={256};return &p; }
static int MicoFlashRead(int p,uint32_t *off,void *bytes,unsigned n) {
 if(read_fault)return -9;assert(*off+n<=256);memcpy(bytes,flash[p]+*off,n);*off+=n;return 0;
}
static int MicoFlashWrite(int p,uint32_t *off,const void *bytes,unsigned n) {
 if(write_fault)return -8;assert(*off+n<=256);memcpy(flash[p]+*off,bytes,n);*off+=n;writes++;return 0;
}
static int MicoFlashErase(int p,uint32_t off,unsigned n) { assert(off+n<=256);memset(flash[p]+off,255,n);return 0; }
static system_context_t *mico_system_context_get(void) { return &context; }
static int mico_system_context_restore(system_context_t *c) { (void)c; restores++;return 0; }
static uint32_t app_config_legacy_size(void) { return 32; }
static uint32_t app_config_legacy_next_size(uint32_t size) { return size==32?16:0; }
static bool app_config_migrate_legacy(void *d,uint32_t size) { assert(size==32||size==16);migrations++;memset((uint8_t*)d+size,0,64-size);return true; }
bool app_config_persist_allowed(void) { return true; }
void app_config_commit_indeterminate(void) {}
'''
        checked_crc = 'static OSStatus para_crc16(' in source
        if checked_crc:
            harness += source[source.index('static bool para_commit_uncertain'):source.index('OSStatus mico_system_context_restore')]
            harness += 'static uint16_t test_crc(int p) { uint16_t crc; assert(para_crc16(p,&crc)==0); return crc; }\n'
        else:
            for signature in ('static uint16_t para_crc16(', 'static OSStatus internal_update_config('):
                harness += function(source, signature)
            harness += 'static uint16_t test_crc(int p) { return para_crc16(p); }\n'
        # Skip the forward declaration of try_old_para.
        harness += function(source[source.index('/* Read the 3.5 CRC'):], 'static OSStatus try_old_para(')
        harness += r'''
static void setup(unsigned size,unsigned slot) {
 memset(flash,255,sizeof flash);memset(data,0x42,sizeof data);
 context=(system_context_t){.flashContentInRam={.micoSystemConfig={SYS_MAGIC_NUMBR,17}},.user_config_data=data,.user_config_data_size=64};
 memcpy(flash[slot],&context.flashContentInRam,32);memcpy(flash[slot]+32,data,size);
 CRC16_Context crc;uint16_t value;CRC16_Init(&crc);CRC16_Update(&crc,flash[slot]+24,8+size);CRC16_Final(&crc,&value);
 memcpy(flash[slot]+32+size,&value,2);memset(data,0,sizeof data);
 writes=restores=migrations=read_fault=write_fault=0;
}
int main(void) {
 for(unsigned size=16;size<=64;size*=2)for(unsigned slot=0;slot<2;slot++) {
  setup(size,slot);assert(try_old_para(&context)==0);assert(writes>0 && !restores);
  assert(migrations==(size!=64));assert(seedNum==17);
  for(unsigned i=0;i<size;i++)assert(data[i]==0x42);
  for(unsigned i=size;i<64;i++)assert(data[i]==0);
  assert(!memcmp(flash[0],flash[1],256));uint16_t stored;memcpy(&stored,flash[0]+254,2);
  assert(stored==test_crc(0));assert(!memcmp(flash[0]+32,data,64));
 }
 setup(32,1);flash[1][40]^=1;assert(try_old_para(&context)==0);assert(restores==1 && !writes && !migrations);
 setup(32,1);flash[1][24]^=1;assert(try_old_para(&context)==0);assert(restores==1 && !writes);
 setup(64,1);read_fault=1;assert(try_old_para(&context)==-9);assert(!restores && !writes);
 setup(32,1);uint8_t backup[256];memcpy(backup,flash[1],256);write_fault=1;
 assert(try_old_para(&context)==-8);assert(!restores && !memcmp(backup,flash[1],256));
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'migration.c').write_text(harness)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            '-Wno-misleading-indentation', str(directory / 'migration.c'),
                            '-o', str(directory / 'migration')], check=True)
            subprocess.run([str(directory / 'migration')], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
