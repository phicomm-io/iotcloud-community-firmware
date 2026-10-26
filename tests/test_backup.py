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
/* Exact legacy typedef from M1/storage.h blob
 * e6d9a64fc117acfd42df93eaaec632d5b115a67d. Keep this fixture independent of
 * the current storage type and its new display member. */
typedef struct {
    m1_config_t base;
    feature_config_storage_t features;
    automation_config_t automation;
    uint8_t reserved_controller_config[20];
    m1_co2_config_t co2;
    m1_co2_source_config_t co2_source;
} legacy_m1_storage_v8_t;
/* Independent v9 fixture: the source lane and complete record are frozen,
 * rather than borrowing the newer display type and truncating its size. */
typedef struct {
    char device_id[13];
    uint8_t source, metric, unit;
    uint16_t scale;
    uint8_t decimals, ttl_seconds, color_mode, color, reserved[2];
    float low, high;
} legacy_m1_display_lane_v9_t;
typedef struct {
    uint32_t magic, revision;
    uint8_t configured, selected, double_click, reserved;
    legacy_m1_display_lane_v9_t lanes[2];
} legacy_m1_display_v9_t;
typedef struct {
    m1_config_t base;
    feature_config_storage_t features;
    automation_config_t automation;
    uint8_t reserved_controller_config[20];
    m1_co2_config_t co2;
    m1_co2_source_config_t co2_source;
    legacy_m1_display_v9_t display;
} legacy_m1_storage_v9_t;
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
static void reject_backup(const void *data,size_t length){
    const app_storage_t before=live;
    const unsigned old_writes=writes,old_finishes=finishes;
    assert(!strcmp(device_backup_import(data,length),"invalid_backup"));
    assert(writes==old_writes&&finishes==old_finishes&&!lock&&!memcmp(&live,&before,sizeof(live)));
}
int main(void){
    assert(sizeof(legacy_m1_storage_v8_t)==2856&&__alignof__(legacy_m1_storage_v8_t)==8);
    assert(sizeof(m1_storage_v8_t)==sizeof(legacy_m1_storage_v8_t));
    assert(sizeof(legacy_m1_display_lane_v9_t)==32&&sizeof(legacy_m1_display_v9_t)==76);
    assert(sizeof(legacy_m1_storage_v9_t)==2928&&sizeof(m1_storage_v9_t)==2928);
    assert(sizeof(m1_storage_v10_t)==2944&&sizeof(app_storage_t)==3064&&offsetof(app_storage_t,display)==2852);
    assert(offsetof(app_storage_t,display.style)==2928);
#define V8_OFFSET(member) assert(offsetof(legacy_m1_storage_v8_t,member)==offsetof(app_storage_t,member))
    V8_OFFSET(base);V8_OFFSET(features);V8_OFFSET(automation);V8_OFFSET(reserved_controller_config);
    V8_OFFSET(co2);V8_OFFSET(co2_source);
#undef V8_OFFSET
    live.base.magic=M1_CONFIG_MAGIC;live.base.version=M1_CONFIG_VERSION;live.base.mqtt_port=1883;
    strcpy(live.base.ssid,"kept-network");strcpy(live.base.wifi_password,"kept-password");
    strcpy(live.base.mqtt_password,"mqtt-secret");strcpy(live.base.web_password,"web-secret");
    live.base.revision=42;live.features.magic=FEATURE_MAGIC;live.automation.magic=AUTOMATION_MAGIC;
    live.co2.magic=M1_CO2_MAGIC;live.co2.interval=5;
    live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    m1_display_defaults(&live.display);
    live.display.configured=live.display.selected=live.display.double_click=1;
    live.display.revision=7;live.display.lanes[0].source=M1_DISPLAY_API;
    live.display.lanes[0].color=2;
    live.features.revision=13;live.features.last_event=UINT64_C(0x123456789abcdef0);
    strcpy(live.features.ntp_host,"clock.example");live.automation.revision=17;
    memset(live.reserved_controller_config,0x6c,sizeof(live.reserved_controller_config));
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
    /* Construct real v8 storage, including its four tail-padding bytes. A new
     * struct prefix ending at display would incorrectly produce only 2852. */
    legacy_m1_storage_v8_t legacy={0};
#define V8_COPY(member) memcpy(&legacy.member,&original.member,sizeof(legacy.member))
    V8_COPY(base);V8_COPY(features);V8_COPY(automation);V8_COPY(reserved_controller_config);
    V8_COPY(co2);V8_COPY(co2_source);
#undef V8_COPY
    legacy.base.version=8;legacy.co2.mode=2;legacy.co2.slot=1;
    legacy.co2_source.external=1;strcpy(legacy.co2_source.device_id,"123456abcdef");
    const size_t padding=offsetof(legacy_m1_storage_v8_t,co2_source)+sizeof(legacy.co2_source);
    assert(padding==2852&&sizeof(legacy)-padding==4);
    memset((char*)&legacy+padding,0xa5,sizeof(legacy)-padding);
    const size_t old_bytes=sizeof(legacy),old_length=sizeof(ph_backup_header)+old_bytes;
    void *old_data=malloc(old_length);assert(old_data);
    memcpy((char*)old_data+sizeof(ph_backup_header),&legacy,sizeof(legacy));
    ph_backup_seal(old_data,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,8,old_bytes);
    assert(ph_backup_open(old_data,old_length,"m1",PHICOMM_FIRMWARE_VARIANT,8,sizeof(legacy)));
    before=live;write_fail=true;assert(!strcmp(device_backup_import(old_data,old_length),"storage_error"));
    assert(!memcmp(&live,&before,sizeof(live))&&writes==3&&finishes==3&&!committed&&!lock);
    write_fail=false;occupied=true;
    assert(!strcmp(device_backup_import(old_data,old_length),"busy")&&writes==3&&!lock);
    assert(!memcmp(&live,&before,sizeof(live)));occupied=false;recovery=true;
    assert(!strcmp(device_backup_import(old_data,old_length),"busy")&&writes==3&&!lock);
    assert(!memcmp(&live,&before,sizeof(live)));recovery=false;
    write_fail=false;assert(!strcmp(device_backup_import(old_data,old_length),"accepted"));
    assert(writes==4&&finishes==4&&committed&&!lock&&live.base.version==M1_CONFIG_VERSION&&live.base.revision==44);
    assert(live.co2.mode==2&&live.co2.slot==1&&live.co2_source.external);
    assert(!strcmp(live.co2_source.device_id,"123456abcdef")&&m1_display_config_valid(&live.display)&&!live.display.configured);
    m1_config_t expected_base;memcpy(&expected_base,&legacy.base,sizeof(expected_base));
    expected_base.version=M1_CONFIG_VERSION;expected_base.revision=44;
    assert(!memcmp(&live.base,&expected_base,sizeof(expected_base)));
#define V8_PRESERVED(member) assert(!memcmp(&live.member,&legacy.member,sizeof(legacy.member)))
    V8_PRESERVED(features);V8_PRESERVED(automation);V8_PRESERVED(reserved_controller_config);
    V8_PRESERVED(co2);V8_PRESERVED(co2_source);
#undef V8_PRESERVED
    m1_display_config_t defaults;m1_display_defaults(&defaults);
    assert(!memcmp(&live.display,&defaults,sizeof(defaults)));
    reject_backup(old_data,old_length-1);
    /* Reject the former fabricated 2852-byte prefix, even with a valid CRC. */
    memcpy(bad,old_data,old_length);
    ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,8,old_bytes-4);
    reject_backup(bad,old_length-4);
    memcpy(bad,old_data,old_length);memset((char*)bad+old_length,0,4);
    ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,8,old_bytes+4);
    reject_backup(bad,old_length+4);
    for(unsigned kind=0;kind<3;kind++){
        memcpy(bad,old_data,old_length);
        ph_backup_seal(bad,kind==0?"a1":"m1",kind==1?"wrong-variant":PHICOMM_FIRMWARE_VARIANT,
                       FIRMWARE_VERSION,kind==2?M1_CONFIG_VERSION:8,old_bytes);
        reject_backup(bad,old_length);
    }
    memcpy(bad,old_data,old_length);
    legacy_m1_storage_v8_t *old_payload=(void*)((char*)bad+sizeof(ph_backup_header));
    old_payload->base.version=M1_CONFIG_VERSION;
    ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,8,old_bytes);
    reject_backup(bad,old_length);
    memcpy(bad,old_data,old_length);old_payload->co2.interval=0;
    ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,8,old_bytes);
    reject_backup(bad,old_length);
    /* The CRC includes arbitrary legacy padding, which becomes display magic
     * only after the migration initializes the whole new profile. */
    ((char*)old_data)[old_length-1]^=1;
    reject_backup(old_data,old_length);
    memcpy(bad,data,length);candidate=(void*)((char*)bad+sizeof(ph_backup_header));candidate->base.version=8;
    ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,M1_CONFIG_VERSION,sizeof(live));
    reject_backup(bad,length);

    /* Import a real v9 payload while a formatted v10 profile is active.
     * New styles must be initialized, not retained from the prior profile. */
    legacy_m1_storage_v9_t v9;
    memcpy(&v9,&original,sizeof(v9));v9.base.version=9;
    v9.display.revision=19;
    v9.display.lanes[0].source=M1_DISPLAY_MQTT;
    strcpy(v9.display.lanes[0].device_id,"123456abcdef");
    v9.display.lanes[0].metric=M1_DISPLAY_CO2;v9.display.lanes[0].unit=M1_DISPLAY_PPM;
    v9.display.lanes[0].scale=1000;v9.display.lanes[0].decimals=2;
    const size_t v9_bytes=sizeof(v9),v9_length=sizeof(ph_backup_header)+v9_bytes;
    void *v9_data=malloc(v9_length);assert(v9_data);
    memcpy((char*)v9_data+sizeof(ph_backup_header),&v9,sizeof(v9));
    ph_backup_seal(v9_data,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,9,v9_bytes);
    assert(m1_display_v9_valid(&v9.display));
    live.display.reserved=M1_DISPLAY_FORMATTED;
    live.display.style[0]=(m1_display_style_t){.lamps=0x155,.mode=M1_DISPLAY_AUTO_KILO,.show_unit=1};
    live.display.style[1]=(m1_display_style_t){.lamps=0x0aa,.mode=M1_DISPLAY_INTEGER,.show_label=1};
    assert(m1_display_config_valid(&live.display));
    before=live;unsigned v9_writes=writes,v9_finishes=finishes;
    write_fail=true;assert(!strcmp(device_backup_import(v9_data,v9_length),"storage_error"));
    assert(writes==v9_writes+1&&finishes==v9_finishes+1&&!committed&&!lock);
    assert(!memcmp(&live,&before,sizeof(live)));
    write_fail=false;occupied=true;
    assert(!strcmp(device_backup_import(v9_data,v9_length),"busy")&&writes==v9_writes+1&&!lock);
    assert(!memcmp(&live,&before,sizeof(live)));occupied=false;
    recovery=true;assert(!strcmp(device_backup_import(v9_data,v9_length),"busy"));
    assert(writes==v9_writes+1&&!memcmp(&live,&before,sizeof(live)));recovery=false;
    assert(!strcmp(device_backup_import(v9_data,v9_length),"accepted"));
    assert(writes==v9_writes+2&&finishes==v9_finishes+2&&committed&&!lock);
    assert(live.base.version==M1_CONFIG_VERSION&&live.base.revision==before.base.revision+1);
    before=live;before.base.version=9;before.base.revision=v9.base.revision;
    assert(!memcmp(&before,&v9,sizeof(v9)));
    assert(!memcmp(live.display.style,(m1_display_style_t[2]){{0},{0}},sizeof(live.display.style)));
    assert(live.display.configured&&live.display.selected&&live.display.double_click);
    assert(live.display.revision==19&&!live.display.reserved&&m1_display_config_valid(&live.display));
    reject_backup(v9_data,v9_length-1);
    for(unsigned kind=0;kind<7;kind++){
        memcpy(bad,v9_data,v9_length);
        legacy_m1_storage_v9_t *old=(void*)((char*)bad+sizeof(ph_backup_header));
        if(kind==0)old->display.reserved=M1_DISPLAY_FORMATTED;
        if(kind==1)old->display.lanes[0].reserved[0]=1;
        if(kind==2)old->display.lanes[0].scale=10;
        if(kind==3)old->display.lanes[0].color=4;
        if(kind==4)strcpy(old->display.lanes[0].device_id,m1_id);
        if(kind==5)old->base.version=M1_CONFIG_VERSION;
        if(kind==6)old->display.revision=UINT32_MAX;
        ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,9,v9_bytes);
        reject_backup(bad,v9_length);
    }
    for(unsigned kind=0;kind<3;kind++){
        memcpy(bad,v9_data,v9_length);
        ph_backup_seal(bad,kind==0?"a1":"m1",kind==1?"wrong-variant":PHICOMM_FIRMWARE_VARIANT,
                       FIRMWARE_VERSION,kind==2?M1_CONFIG_VERSION+1:9,v9_bytes);
        reject_backup(bad,v9_length);
    }
    memcpy(bad,v9_data,v9_length);ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,9,v9_bytes-4);
    reject_backup(bad,v9_length-4);
    memcpy(bad,v9_data,v9_length);memset((char*)bad+v9_length,0,4);
    ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,9,v9_bytes+4);
    reject_backup(bad,v9_length+4);

    /* An exact-size v10 backup retains its presentation and gains canonical
     * field bindings, with no dependence on bytes outside the old payload. */
    m1_storage_v10_t v10;memcpy(&v10,&live,sizeof(v10));v10.base.version=10;
    v10.display.prefix.reserved=M1_DISPLAY_FORMATTED;
    v10.display.prefix.lanes[0].color=v10.display.prefix.lanes[0].decimals=0;
    v10.display.prefix.lanes[0].scale=1;
    v10.display.style[0]=(m1_display_style_t){.lamps=0x155,.mode=M1_DISPLAY_AUTO_KILO,.show_unit=1};
    v10.display.style[1]=(m1_display_style_t){.lamps=0xaa,.show_label=1};
    const size_t v10_length=sizeof(ph_backup_header)+sizeof(v10);
    void *v10_data=malloc(v10_length);assert(v10_data);
    memcpy((char*)v10_data+sizeof(ph_backup_header),&v10,sizeof(v10));
    ph_backup_seal(v10_data,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,10,sizeof(v10));
    before=live;write_fail=true;assert(!strcmp(device_backup_import(v10_data,v10_length),"storage_error"));
    assert(!memcmp(&live,&before,sizeof(live)));write_fail=false;
    assert(!strcmp(device_backup_import(v10_data,v10_length),"accepted"));
    assert(live.base.version==M1_CONFIG_VERSION&&live.display.revision==v10.display.prefix.revision);
    assert(!strcmp(live.display.remote[0].field_id,"co2")&&!strcmp(live.display.remote[0].unit,"ppm"));
    assert(!memcmp(live.display.style,v10.display.style,sizeof(v10.display.style)));
    for(unsigned kind=0;kind<4;kind++){
        memcpy(bad,v10_data,v10_length);m1_storage_v10_t *old=(void*)((char*)bad+sizeof(ph_backup_header));
        if(kind==0)old->display.style[1].lamps=512;
        if(kind==1)old->display.prefix.lanes[0].metric=M1_DISPLAY_CUSTOM;
        if(kind==2)old->display.prefix.reserved=2;
        if(kind==3)old->base.version=M1_CONFIG_VERSION;
        ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,10,sizeof(v10));reject_backup(bad,v10_length);
    }
    free(v10_data);

    /* Current backups include all independently configured lamp segments and
     * preserve them through Flash rollback and a successful full round-trip. */
    live.display.reserved=M1_DISPLAY_FORMATTED;
    live.display.lanes[0].color=live.display.lanes[0].decimals=0;
    live.display.lanes[0].scale=1;
    live.display.style[0]=(m1_display_style_t){.lamps=0x155,.mode=M1_DISPLAY_MILLI,.show_unit=1};
    live.display.style[1]=(m1_display_style_t){.lamps=0x0aa,.mode=M1_DISPLAY_INTEGER,.show_label=1};
    assert(m1_display_config_valid(&live.display));
    assert(device_backup_export(data,length));const app_storage_t formatted=live;
    memset(live.display.style,0,sizeof(live.display.style));before=live;
    write_fail=true;assert(!strcmp(device_backup_import(data,length),"storage_error"));
    assert(!memcmp(&live,&before,sizeof(live))&&!lock);write_fail=false;
    assert(!strcmp(device_backup_import(data,length),"accepted"));
    before=formatted;before.base.revision=formatted.base.revision+1;
    assert(!memcmp(&live,&before,sizeof(live))&&!lock);
    memcpy(bad,data,length);candidate=(void*)((char*)bad+sizeof(ph_backup_header));
    candidate->display.style[1].lamps=512;
    ph_backup_seal(bad,"m1",PHICOMM_FIRMWARE_VARIANT,FIRMWARE_VERSION,M1_CONFIG_VERSION,sizeof(live));
    reject_backup(bad,length);
    puts("M1 backup: exact v8/v9/v10 payloads, v11 generic bindings, strict envelope and rollback PASS");
    free(v9_data);
    free(old_data);
    free(data);free(bad);return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source_file, binary = Path(temporary)/'adapter.c', Path(temporary)/'adapter'
            source_file.write_text(source)
            subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                            '-I',str(ROOT/'tests/host'),'-I',str(ROOT),'-I',str(ROOT/'M1'),
                            '-I',str(Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT/'build/m1-sdk/mico-os/libraries/utilities/json_c'))),
                            str(source_file),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
