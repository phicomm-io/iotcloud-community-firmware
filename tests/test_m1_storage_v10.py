"""Run M1's actual layout gate and v9->v10 boot migration with Flash faults."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class M1StorageV10Test(unittest.TestCase):
    def test_boot_prefix_preservation_and_failed_commit(self):
        source = (ROOT / 'M1/main.c').read_text()
        gate_start = source.index('    if (app_config->magic != M1_CONFIG_MAGIC')
        migration_start = source.index('    bool migration_changed', gate_start)
        migration_end = source.index('    err = mico_system_init', migration_start)
        callbacks = source[source.index('void appRestoreDefault_callback'):source.index('void m1_ntp_host_get')]
        harness = r'''
#include "M1/storage.h"
#include "M1/config_legacy.h"
static app_storage_t live,flash;
static m1_config_t *app_config=&live.base;
mico_Context_t *m1_context=(mico_Context_t *)(uintptr_t)1;
static mico_mutex_t config_lock;
static bool config_recovery,fail_flash;
static unsigned writes,unsupported,migration_failures;
void feature_defaults(feature_config_t *out){memset(out,0,sizeof(*out));out->magic=FEATURE_MAGIC;}
void automation_defaults(automation_config_t *out){memset(out,0,sizeof(*out));out->magic=AUTOMATION_MAGIC;}
static int persist(void *context){assert(context==m1_context);writes++;if(fail_flash)return -1;flash=live;return 0;}
#undef mico_system_context_update
#define mico_system_context_update persist
CALLBACKS
void service_event(const char *level,const char *module,const char *event){
    assert(!strcmp(level,"error")&&!strcmp(module,"config"));
    if(!strcmp(event,"unsupported_layout"))unsupported++;
    else {assert(!strcmp(event,"migration_failed"));migration_failures++;}
}
static int boot(void){m1_config_t c;GATE MIGRATION return config_recovery?-1:0;}
static void seed(unsigned version,bool configured){
    memset(&live,0,sizeof(live));appRestoreDefault_callback(&live,sizeof(live));
    live.base.version=version;live.base.revision=37;
    strcpy(live.base.ssid,"kept-network");strcpy(live.base.wifi_password,"kept-wifi-key");
    strcpy(live.base.mqtt_host,"broker.example");strcpy(live.base.mqtt_user,"kept-owner");
    strcpy(live.base.mqtt_password,"kept-mqtt-key");strcpy(live.base.web_password,"kept-web-key");
    strcpy(live.base.name,"kept-name");
    live.features.language=PH_LANG_EN;live.features.revision=19;
    live.features.last_event=UINT64_C(0xfedcba9876543210);strcpy(live.features.ntp_host,"clock.example");
    live.automation.revision=11;memset(live.reserved_controller_config,0x5a,sizeof(live.reserved_controller_config));
    live.co2.mode=2;live.co2.slot=1;live.co2.interval=13;
    live.co2_source.external=live.co2_source.double_click=1;live.co2_source.revision=23;
    strcpy(live.co2_source.device_id,"123456abcdef");
    live.display.configured=configured;live.display.selected=live.display.double_click=1;
    live.display.revision=29;
    live.display.lanes[0]=(m1_display_lane_config_t){.source=M1_DISPLAY_API,.metric=M1_DISPLAY_CO2,
        .unit=M1_DISPLAY_PPM,.scale=1000,.decimals=2,.ttl_seconds=17,.color_mode=M1_DISPLAY_COLOR_THRESHOLDS,
        .low=1000,.high=1500};
    live.display.lanes[1]=(m1_display_lane_config_t){.source=M1_DISPLAY_MQTT,.metric=M1_DISPLAY_FAN_SPEED_PERCENT,
        .unit=M1_DISPLAY_PERCENT,.scale=1,.ttl_seconds=19,.color_mode=M1_DISPLAY_COLOR_FIXED,.color=2};
    strcpy(live.display.lanes[1].device_id,"123456abcdef");
    /* Bytes outside the old on-flash size are deliberately invalid styles. */
    memset(live.display.style,0xa5,sizeof(live.display.style));
    flash=live;config_recovery=fail_flash=false;writes=unsupported=migration_failures=0;
}
static void assert_v9_prefix(const app_storage_t *before){
    app_storage_t expected=*before;expected.base.version=M1_CONFIG_VERSION;
    assert(!memcmp(&live,&expected,sizeof(m1_storage_v9_t)));
    assert(!memcmp(live.display.style,(m1_display_style_t[2]){{0},{0}},sizeof(live.display.style)));
    assert(m1_display_config_valid(&live.display)&&!live.display.reserved);
    assert(!memcmp(&flash,&live,sizeof(live)));
}
int main(void){
    assert(M1_CONFIG_VERSION==10&&sizeof(m1_storage_v8_t)==2856);
    assert(sizeof(m1_storage_v9_t)==2928&&sizeof(app_storage_t)==2944);
    assert(offsetof(app_storage_t,display.style)==sizeof(m1_storage_v9_t));
    assert(sizeof(m1_display_style_t)==8&&sizeof(m1_display_lane_config_t)==32);
    for(unsigned configured=0;configured<2;configured++){
        seed(9,configured);const app_storage_t before=live;
        /* A helper called on an exact-size allocation must not inspect the
         * absent v10 tail. ASan guards the byte immediately after v9. */
        void *exact=malloc(sizeof(m1_display_config_v9_t));assert(exact);
        memcpy(exact,&live.display,sizeof(m1_display_config_v9_t));
        assert(m1_display_v9_valid(exact));free(exact);
        assert(boot()==0&&writes==1&&!config_recovery&&!unsupported&&!migration_failures);
        assert_v9_prefix(&before);
        const app_storage_t current=live;assert(boot()==0&&writes==1);
        assert(!memcmp(&current,&live,sizeof(live)));
    }
    seed(9,true);const app_storage_t old_flash=flash;fail_flash=true;
    assert(boot()!=0&&config_recovery&&writes==1&&!unsupported&&migration_failures==1);
    assert(!memcmp(&flash,&old_flash,sizeof(flash)));
    assert(live.base.version==M1_CONFIG_VERSION&&!live.display.configured);
    assert(!live.base.ssid[0]&&!live.base.mqtt_password[0]);
    assert(boot()!=0&&writes==1); /* recovery does not retry an implicit write */
    live=flash;config_recovery=fail_flash=false;assert(boot()==0&&writes==2);
    assert_v9_prefix(&old_flash);

    /* Old/future discriminators and invalid old lanes cannot be silently
     * converted or persisted under a new valid version number. */
    for(unsigned kind=0;kind<8;kind++){
        seed(9,true);
        if(kind==0)live.display.reserved=M1_DISPLAY_FORMATTED;
        if(kind==1)live.display.lanes[0].scale=10;
        if(kind==2)live.display.lanes[1].device_id[0]='G';
        if(kind==3)live.display.lanes[0].reserved[1]=1;
        if(kind==4)live.display.revision=UINT32_MAX;
        if(kind==5)live.display.lanes[0].low=INFINITY;
        if(kind==6)live.base.version=M1_CONFIG_VERSION+1;
        if(kind==7)live.display.lanes[0].source=M1_DISPLAY_NONE;
        flash=live;const app_storage_t original=flash;
        assert(boot()!=0&&config_recovery&&!writes&&unsupported==1&&!migration_failures);
        assert(!memcmp(&flash,&original,sizeof(flash)));
    }

    seed(8,true);const app_storage_t before_v8=live;
    memset(&live.display,0xa5,sizeof(live.display));flash=live;
    assert(boot()==0&&writes==1&&!config_recovery&&!unsupported);
    app_storage_t expected=before_v8;expected.base.version=M1_CONFIG_VERSION;
    assert(!memcmp(&live,&expected,offsetof(app_storage_t,display)));
    m1_display_config_t defaults;m1_display_defaults(&defaults);
    assert(!memcmp(&live.display,&defaults,sizeof(defaults)));

    /* Booting a valid current formatted profile neither rewrites it nor
     * projects its configured segments back into the old scalar profile. */
    seed(M1_CONFIG_VERSION,true);live.display.reserved=M1_DISPLAY_FORMATTED;
    live.display.lanes[0].scale=1;live.display.lanes[0].decimals=0;
    live.display.lanes[1].color=0;
    live.display.style[0]=(m1_display_style_t){.lamps=0x155,.mode=M1_DISPLAY_AUTO_KILO,.show_unit=1};
    live.display.style[1]=(m1_display_style_t){.lamps=0x0aa,.mode=M1_DISPLAY_INTEGER,.show_label=1};
    assert(m1_display_config_valid(&live.display));const app_storage_t current=live;
    assert(boot()==0&&!writes&&!unsupported&&!migration_failures);
    assert(!memcmp(&current,&live,sizeof(live)));
    puts("M1 v10 boot: frozen v9 prefix, append-only styles, unsupported recovery and Flash failures PASS");
}
'''.replace('CALLBACKS', callbacks).replace('GATE', source[gate_start:migration_start]).replace('MIGRATION', source[migration_start:migration_end])
        with tempfile.TemporaryDirectory() as temporary:
            probe, binary = Path(temporary) / 'probe.c', Path(temporary) / 'probe'
            probe.write_text(harness)
            json_dir = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT/'build/m1-sdk/mico-os/libraries/utilities/json_c'))
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                            '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                            '-I'+str(ROOT), '-I'+str(ROOT/'tests/host'), '-I'+str(ROOT/'M1'), '-I'+str(json_dir),
                            str(probe), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
