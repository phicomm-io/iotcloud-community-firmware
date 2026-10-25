"""Exercise the backup envelope against corruption and incompatible restores."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BackupTest(unittest.TestCase):
    def test_roundtrip_corruption_and_scope(self):
        source = r'''
#include <assert.h>
#include "common/backup.h"
#include "common/mqtt/bemfa.h"
int main(void){
    assert(sizeof(bemfa_config_t)==592 && offsetof(bemfa_config_t,language)==590);
    unsigned char data[sizeof(ph_backup_header)+256], original[sizeof(data)];
    for(unsigned i=sizeof(ph_backup_header);i<sizeof(data);i++)data[i]=(i*37)&255;
    ph_backup_seal(data,"m1","full","1.0.0-abcdef",7,256);
    memcpy(original,data,sizeof(data));
    assert(ph_backup_open(data,sizeof(data),"m1","full",7,256));
    for(unsigned i=0;i<sizeof(data);i++){
        data[i]^=1;
        /* Firmware build text is metadata, outside the payload CRC. */
        if(i>=sizeof(ph_backup_header))assert(!ph_backup_open(data,sizeof(data),"m1","full",7,256));
        memcpy(data,original,sizeof(data));
    }
    assert(!ph_backup_open(data,sizeof(data)-1,"m1","full",7,256));
    assert(!ph_backup_open(data,sizeof(data),"a1","full",7,256));
    assert(!ph_backup_open(data,sizeof(data),"m1","no-frontend-resources",7,256));
    assert(!ph_backup_open(data,sizeof(data),"m1","full",8,256));
    assert(!ph_backup_open(data,sizeof(data),"m1","full",7,255));
    ph_backup_header *h=(void *)data;h->schema=2;
    assert(!ph_backup_open(data,sizeof(data),"m1","full",7,256));
    memcpy(data,original,sizeof(data));h->length++;
    assert(!ph_backup_open(data,sizeof(data),"m1","full",7,256));
    memcpy(data,original,sizeof(data));memset(h->model,'m',sizeof(h->model));
    assert(!ph_backup_open(data,sizeof(data),"m1","full",7,256));
    return 0;
}'''
        with tempfile.TemporaryDirectory() as temporary:
            source_file, binary = Path(temporary)/'backup.c', Path(temporary)/'backup'
            source_file.write_text(source)
            subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT),str(source_file),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


    def test_production_m1_adapter_transaction(self):
        source = r'''
#include <assert.h>
#include "M1/storage.h"
#include "common/backup.h"
static app_storage_t live;
char m1_id[13]="aabbccddeeff";
static mico_mutex_t lock;
static unsigned writes, finishes;
static bool recovery, occupied, write_fail, committed;
bool device_config_recovery(void){return recovery;}
void device_config_recovery_set(bool value){recovery=value;}
static int persist(void *context){assert(context==&live && lock);writes++;return write_fail?-1:0;}
static bool backup_reserve(void){return !occupied;}
static void backup_finish(bool ok){finishes++;committed=ok;}
#define mico_system_context_update persist
#define BACKUP_MODEL "m1"
#define BACKUP_BASE m1_config_t
#define BACKUP_MAGIC M1_CONFIG_MAGIC
#define BACKUP_VERSION M1_CONFIG_VERSION
#define BACKUP_LIVE (&live.base)
#define BACKUP_LOCK lock
#define BACKUP_CONTEXT (&live)
#include "common/backup_mico.inc"
int main(void){
    live.base.magic=M1_CONFIG_MAGIC;live.base.version=M1_CONFIG_VERSION;live.base.mqtt_port=1883;
    strcpy(live.base.ssid,"kept-network");strcpy(live.base.wifi_password,"kept-password");
    strcpy(live.base.mqtt_password,"mqtt-secret");strcpy(live.base.web_password,"web-secret");
    live.base.revision=42;live.features.magic=FEATURE_MAGIC;live.automation.magic=AUTOMATION_MAGIC;
    live.co2.magic=M1_CO2_MAGIC;live.co2.interval=5;
    live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    const size_t length=device_backup_size();assert(length<=PH_BACKUP_LIMIT);
    void *data=malloc(length),*bad=malloc(length);assert(data&&bad);
    assert(device_backup_export(data,length));
    const app_storage_t original=live;
    strcpy(live.base.ssid,"other-network");live.features.language=PH_LANG_EN;
    app_storage_t before=live;
    write_fail=true;assert(!strcmp(device_backup_import(data,length),"storage_error"));
    assert(writes==1&&finishes==1&&!committed&&!memcmp(&live,&before,sizeof(live))&&!lock);
    occupied=true;assert(!strcmp(device_backup_import(data,length),"busy"));assert(writes==1&&!lock);
    occupied=false;recovery=true;assert(!device_backup_export(bad,length));
    assert(!strcmp(device_backup_import(data,length),"busy"));assert(writes==1);
    recovery=false;memcpy(bad,data,length);((unsigned char*)bad)[length-1]^=1;
    assert(!strcmp(device_backup_import(bad,length),"invalid_backup"));assert(writes==1);
    memcpy(bad,data,length);app_storage_t *candidate=(void*)((char*)bad+sizeof(ph_backup_header));
    candidate->features.language=99;
    ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,M1_CONFIG_VERSION,sizeof(live));
    assert(!strcmp(device_backup_import(bad,length),"invalid_backup"));assert(writes==1);
    write_fail=false;assert(!strcmp(device_backup_import(data,length),"accepted"));
    assert(writes==2&&finishes==2&&committed&&!lock);
    before=original;before.base.revision=43;assert(!memcmp(&live,&before,sizeof(live)));
    free(data);free(bad);return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source_file, binary = Path(temporary)/'adapter.c', Path(temporary)/'adapter'
            source_file.write_text(source)
            subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                            '-I',str(ROOT/'tests/host'),'-I',str(ROOT),'-I',str(ROOT/'M1'),
                            '-I',str(ROOT/'build/m1-sdk/mico-os/libraries/utilities/json_c'),
                            str(source_file),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
