#!/usr/bin/env python3
"""Exercise the patched OpenBeken OTA callback with an actual built RBL.

Compile its flash writer and callback verbatim; replace only hardware services.
This checks the upstream error -> end callback sequence, not just the guard.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

STUBS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ota_guard.h"
typedef unsigned char UINT8;
typedef unsigned int UINT32;
typedef int PROTECT_TYPE;
#define START_ADR_OF_BK_PARTITION_OTA 0x12a000u
#define LFS_BLOCKS_END 0x1d0000u
#define LFS_START 0x1c8000u
#define FLASH_XTX_16M_SR_WRITE_ENABLE 0
#define FLASH_PROTECT_NONE 0
#define FLASH_UNPROTECT_LAST_BLOCK 1
#define CMD_FLASH_WRITE_ENABLE 0
#define CMD_FLASH_ERASE_SECTOR 1
#define LOG_INFO 0
#define LOG_ERROR 1
#define LOG_FEATURE_OTA 0
#define ENABLE_BL_SHARED 0
#define addLogAdv(...) ((void)0)
static unsigned char flash[0x200000];
static unsigned reboots, progress, total_bytes, protection, erases;
static bool erased[512];
static int fail_alloc;
static void *os_malloc(size_t n) { return fail_alloc ? NULL : malloc(n); }
#define os_free free
static void flash_init(void) {}
void flash_protection_op(UINT8 mode, PROTECT_TYPE type) {(void)mode;protection=type;}
UINT32 flash_read(char *p, UINT32 n, UINT32 a) {assert(a+n<=sizeof flash);memcpy(p,flash+a,n);return 0;}
UINT32 flash_write(char *p, UINT32 n, UINT32 a) {
    assert(n<=512 && a>=START_ADR_OF_BK_PARTITION_OTA && a+n<=LFS_START && erased[a/4096]);
    memcpy(flash+a,p,n);return 0;
}
UINT32 flash_ctrl(UINT32 cmd, void *arg) {
    if(cmd==CMD_FLASH_ERASE_SECTOR){unsigned a=*(unsigned*)arg;
        assert(!(a%4096) && a>=START_ADR_OF_BK_PARTITION_OTA && a+4096<=LFS_START);
        memset(flash+a,255,4096);erased[a/4096]=true;++erases;
    }return 0;
}
static unsigned CFG_GetLFS_Size(void) {return 32768;}
static void OTA_SetTotalBytes(unsigned n) {total_bytes=n;}
static unsigned OTA_GetTotalBytes(void) {return total_bytes;}
static void OTA_IncrementProgress(unsigned n) {progress+=n;}
static void OTA_ResetProgress(void) {progress=0;}
static void CFG_IncrementOTACount(void) {}
static void CFG_Save_IfThereArePendingChanges(void) {}
static void DRV_SavePowerMeterDriverStatistics(void) {}
static void rtos_delay_milliseconds(unsigned n) {(void)n;}
void bk_reboot(void) {++reboots;}
typedef struct {int response_code;} httpclient_t;
typedef struct {int response_content_len,response_buf_filled,response_buf_len;char *response_buf;} httpclient_data_t;
typedef struct {int state;httpclient_t client;httpclient_data_t client_data;} httprequest_t;
'''

SCENARIOS = r'''
static void untouched(void) {
    for(unsigned i=0;i<START_ADR_OF_BK_PARTITION_OTA;++i)assert(flash[i]==0xa5);
    for(unsigned i=LFS_START;i<sizeof flash;++i)assert(flash[i]==0xa5);
}
static void begin(unsigned n, int status) {
    assert(!sector && !http_buf);memset(flash,0xa5,sizeof flash);reboots=erases=progress=total_bytes=0;
    memset(erased,0,sizeof erased);http_buf=malloc(513);assert(http_buf);
    memset(&httprequest,0,sizeof httprequest);
    httprequest.client.response_code=status;
    httprequest.client_data.response_content_len=(int)n;
    assert(myhttpclientcallback(&httprequest)==0);
}
static int chunk(const unsigned char *p, unsigned n) {
    httprequest.state=1;httprequest.client_data.response_buf=(char*)p;
    httprequest.client_data.response_buf_filled=(int)n;
    return myhttpclientcallback(&httprequest);
}
static void end(int success) {
    httprequest.state=2;httprequest.client_data.response_buf_filled=0;
    assert(myhttpclientcallback(&httprequest)==0);
    assert(reboots==(unsigned)success && !sector && !http_buf && !httprequest.client_data.response_buf && protection==FLASH_UNPROTECT_LAST_BLOCK);
    if(success)assert(progress>=OTA_GetTotalBytes() && total_bytes>0);
    else assert(progress==0);
    untouched();
    if(!success)for(unsigned i=0;i<4096;++i)assert(flash[START_ADR_OF_BK_PARTITION_OTA+i]==255 || fail_alloc);
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long length=ftell(f);assert(length>8192 && length<600000);
    rewind(f);unsigned n=(unsigned)length;unsigned char *rbl=malloc(n);assert(rbl);
    assert(fread(rbl,1,n,f)==n);fclose(f);
    begin(n,200);
    /* Match the real 10-second timeout: bytes arrive, then error and end. */
    assert(!chunk(rbl,8192));httprequest.state=-2;assert(!myhttpclientcallback(&httprequest));end(0);
    begin(n,200);assert(!chunk(rbl,8192));end(0); /* Silent short body. */
    begin(n,404);assert(chunk(rbl,2048)==1);end(0);
    begin(LFS_START-START_ADR_OF_BK_PARTITION_OTA+1,200);assert(chunk(rbl,2048)==1);end(0);
    begin(0,200);assert(chunk(rbl,2048)==1);end(0);
    begin(n,200);unsigned char bad[96]={0};assert(chunk(bad,sizeof bad)==1);end(0);
    fail_alloc=1;begin(n,200);assert(chunk(rbl,2048)==1);end(0);fail_alloc=0;
    /* A fresh complete request succeeds after all the failure paths. */
    begin(n,200);
    for(unsigned offset=0;offset<n;){unsigned take=n-offset;if(take>2047)take=2047;
        assert(!chunk(rbl+offset,take));offset+=take;}
    end(1);assert(!memcmp(flash+START_ADR_OF_BK_PARTITION_OTA,rbl,n));
    assert(erases==(n+4095)/4096);
    for(unsigned i=n;i<((n+4095)&~4095u);++i)assert(flash[START_ADR_OF_BK_PARTITION_OTA+i]==255);
    free(rbl);puts("PASS: native OTA callback: 7 rejection cases, complete RBL, retry and flash boundaries");return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-source', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    path = args.app_source/'src/hal/bk7231/hal_ota_bk7231.c'
    source = path.read_text()
    native = source[source.index('static unsigned char *sector'):source.index('  // NOTE: these MUST persist')]
    (run/'ota_callback.c').write_text(STUBS+native+SCENARIOS)
    compiler = shutil.which('clang')
    if not compiler:
        raise SystemExit('Host clang is required')
    executable = run/'ota-callback-tests'
    with (run/'compiler.log').open('w') as log:
        subprocess.run([compiler, '-std=c99', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(Path(__file__).resolve().parents[1]/'src'),
                        str(run/'ota_callback.c'), '-o', str(executable)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    with (run/'test.log').open('w') as log:
        subprocess.run([str(executable), str(args.firmware.resolve())],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    (run/'result.json').write_text(json.dumps({'status': 'PASS', 'source': str(path),
                                             'firmware': str(args.firmware.resolve()),
                                             'sanitizers': ['address', 'undefined'],
                                             'hardware_verified': False}, indent=2)+'\n')
    print((run/'test.log').read_text(), end='')


if __name__ == '__main__':
    main()
