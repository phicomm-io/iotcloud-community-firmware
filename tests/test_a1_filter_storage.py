"""Run production storage/service functions with a fallible RAM Flash driver."""
import os
from pathlib import Path
from tests.test_http_json_budget import function
import subprocess
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FilterStorageTest(unittest.TestCase):
    def test_storage_commands_and_migration(self):
        main = (ROOT / 'A1/main.c').read_text()
        prefix = main[main.index('static bool config_recovery'):main.index('#include "../common/service.h"')] + main[main.index('mico_mutex_t a1_lock'):main.index('static OSStatus startup_result')] + main[main.index('void automation_load('):main.index('#include "../common/device_status.h"')]
        migration = main[main.index('    /* The SDK validates'):main.index('#ifdef A1_FRESH_AIR\n    err=startup_result("fresh air GPIO"')]
        weight = function((ROOT / 'common/runtime.c').read_text(), 'unsigned feature_filter_weight(')
        harness = r'''
#define A1_FRESH_AIR
#include "A1/storage.h"
#include "A1/config_legacy.h"
#include <stddef.h>
#include <math.h>
static feature_config_t configuration;
static unsigned data_fields;
static float aggregate[4];
static int weight_locked;
static void lock(void){assert(!weight_locked);weight_locked=1;}
static void unlock(void){assert(weight_locked);weight_locked=0;}
WEIGHT
static uint32_t now;
static int writes, fail_write, sdk_write;
bool app_config_persist_allowed(void);
static app_storage_t flash;
uint32_t mico_rtos_get_time(void){return now;}
static int flash_update(void *ctx){(void)ctx;writes++;if(fail_write)return -1;extern a1_config_t *a1_config;memcpy(&flash,a1_config,sizeof(flash));return 0;}
#define mico_system_context_update flash_update
bool feature_fan_auto(void){return false;}
void feature_co2_defaults(feature_co2_config_t *c){*c=(feature_co2_config_t){1500,1000,800,2000};}
void feature_defaults(feature_config_t *c){memset(c,0,sizeof(*c));c->magic=FEATURE_MAGIC;feature_co2_defaults(&c->co2);}
void automation_defaults(automation_config_t *c){memset(c,0,sizeof(*c));c->magic=AUTOMATION_MAGIC;c->next_id=1;c->buttons[0][1].kind=AUTO_FAN_MODE_TOGGLE;}
void service_event(const char *l,const char *m,const char *e){(void)l;(void)m;(void)e;}
static int startup_result(const char *s,int err){(void)s;return err;}
static int mico_system_init(void *ctx){(void)ctx;if(sdk_write)assert(!app_config_persist_allowed());return 0;}
static void mico_wlan_get_mac_address(uint8_t *mac){const uint8_t value[]={0xaa,0xbb,0xcc,0xdd,0xee,0xff};memcpy(mac,value,6);}
PRODUCTION
static int migrate(void){a1_config_t c;OSStatus err;MIGRATION return config_recovery?-1:0;}
json_object *a1_status_json(void){return json_object_new_object();}
json_object *a1_status_live_json(void){return json_object_new_object();}
bool a1_panel_inspect(void){return false;}
json_object *fresh_command(const char *a,json_object *p,const char **c){(void)a;(void)p;*c="unsupported";return NULL;}
bool a1_scan_start(void){return false;}
bool a1_wifi_trial(const a1_config_t *c){(void)c;return false;}
bool a1_ota_download(const char *u){(void)u;return false;}
bool a1_control_json(const char *k,json_object *p){(void)k;(void)p;return false;}
bool a1_control_apply(a1_control_t *r,bool b,bool g){(void)r;(void)b;(void)g;return false;}
bool a1_control_light_percent(json_object *p){(void)p;return false;}
int feature_light_select(unsigned effect,json_object **response){(void)effect;(void)response;return 400;}
json_object *feature_json(const char *d){(void)d;return NULL;}
int feature_set(const char *d,json_object *p,json_object **o){(void)d;(void)p;(void)o;return 400;}
json_object *service_parse(const void *data,size_t n){char *copy=strndup(data,n);json_object *o=json_tokener_parse(copy);free(copy);return o;}
#include "common/service_product.inc"
static app_storage_t live;
static void init(void){
 memset(&live,0,sizeof(live));memset(&a1_state,0,sizeof(a1_state));memset(&filter_clock,0,sizeof(filter_clock));memset(&stock_clock,0,sizeof(stock_clock));
 a1_config=&live.base;appRestoreDefault_callback(&live,sizeof(live));
 fresh_defaults(&live.fresh_air);bemfa_defaults(&live.bemfa,"a1","aabbccddeeff");strcpy(live.base.name,"A1-EEFF");
 config_recovery=config_initializing=config_update_pending=false;
 save_at=0;writes=fail_write=sdk_write=0;now=0;
}
static const char *cmd(const char *action,const char *params){
 json_object *p=json_tokener_parse(params);const char *code;json_object *out=service_product(action,p,&code);
 if(out)json_object_put(out);json_object_put(p);return code;
}
int main(void){
 configuration.light.quality_pm=75;configuration.light.quality_hcho=.1f;
 data_fields=0;aggregate[2]=1000;aggregate[3]=10;assert(feature_filter_weight()==15);
 data_fields=4;aggregate[2]=75;assert(feature_filter_weight()==15);
 aggregate[2]=150;assert(feature_filter_weight()==75);aggregate[2]=151;assert(feature_filter_weight()==150);
 data_fields=8;aggregate[3]=.15f;assert(feature_filter_weight()==75);
 aggregate[3]=.21f;assert(feature_filter_weight()==150);
 data_fields=12;aggregate[2]=0;assert(feature_filter_weight()==150);
 configuration.light.quality_hcho=0;assert(feature_filter_weight()==15 && !weight_locked);
 init();live.filter.used_seconds=123;automation_config_t mapping,check;
 automation_load(&mapping);mapping.revision++;mapping.buttons[2][0].kind=AUTO_FILTER_RESET;
 mapping.buttons[1][1].kind=AUTO_REBOOT;
 assert(automation_save(&mapping));assert(flash.combination_buttons[0].kind==AUTO_FILTER_RESET);
 assert(flash.filter.used_seconds==123 && live.filter.used_seconds==123);
 fail_write=1;mapping.revision++;mapping.buttons[2][0].kind=AUTO_LIGHT_EFFECT;
 assert(!automation_save(&mapping));automation_load(&check);
 assert(check.buttons[2][0].kind==AUTO_FILTER_RESET && check.revision==mapping.revision-1);
 assert(live.filter.used_seconds==123 && live.automation.buttons[1][1].kind==AUTO_REBOOT);
 fail_write=0;live=flash;automation_load(&check);
 assert(check.buttons[2][0].kind==AUTO_FILTER_RESET && check.buttons[1][1].kind==AUTO_REBOOT);
 init();a1_filter_tick(true);now=3599000;assert(a1_config_flush(false));assert(writes==0);
 now=3600000;assert(a1_config_flush(false));assert(writes==1 && flash.filter.used_seconds==3600);
 now=3601000;a1_filter_tick(false);assert(a1_config_flush(false));assert(writes==2 && flash.filter.used_seconds==3601);
 now+=10000;assert(a1_config_flush(false));assert(writes==2);
 a1_filter_tick(true);now+=5000;assert(a1_reboot_later(100));assert(flash.filter.used_seconds==3606);
 live=flash;memset(&filter_clock,0,sizeof(filter_clock));memset(&stock_clock,0,sizeof(stock_clock));memset(&a1_state,0,sizeof(a1_state));
 filter_clock.saved_seconds=live.filter.used_seconds;assert(live.filter.used_seconds==3606);
 assert(!strcmp(cmd("filter.get","{}"),"ok"));
 assert(!strcmp(cmd("filter.set","{\"lifetime_hours\":6000}"),"accepted"));
 assert(live.filter.lifetime_hours==6000 && live.filter.used_seconds==3606);
 const char *bad[]={"{}","{\"lifetime_hours\":0}","{\"lifetime_hours\":20001}","{\"lifetime_hours\":true}","{\"lifetime_hours\":2.5}","{\"lifetime_hours\":\"4320\"}","{\"lifetime_hours\":100,\"other\":1}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!strcmp(cmd("filter.set",bad[i]),"invalid_params"));
 assert(!strcmp(cmd("filter.reset","{\"other\":1}"),"invalid_params"));
 a1_state.ota_active=true;assert(!strcmp(cmd("filter.reset","{}"),"busy"));a1_state.ota_active=false;
 fail_write=1;assert(!strcmp(cmd("filter.reset","{}"),"storage_error"));assert(live.filter.used_seconds==3606);
 assert(!strcmp(cmd("filter.set","{\"lifetime_hours\":100}"),"storage_error"));assert(live.filter.lifetime_hours==6000);
 fail_write=0;assert(!strcmp(cmd("filter.reset","{}"),"accepted"));assert(!live.filter.used_seconds && flash.filter.lifetime_hours==6000 && !flash.filter.used_seconds);
 a1_filter_tick(true);now+=1000;a1_filter_tick(false);assert(live.filter.used_seconds==1);
 assert(a1_reset(false));assert(live.filter.used_seconds==1 && flash.filter.used_seconds==1);
 a1_state.reboot_pending=false;assert(a1_reset(true));assert(!flash.filter.used_seconds && flash.filter.lifetime_hours==4320);
 init();a1_filter_tick(true);now=3600000;fail_write=1;assert(!a1_config_flush(false));int before=writes;
 now+=100;assert(a1_config_flush(false));assert(writes==before);now+=2000;fail_write=0;assert(a1_config_flush(false));assert(writes==before+1);
 for(unsigned v=1;v<=6;v++){
  init();live.base.version=v;strcpy(live.base.ssid,"preserved-wifi");strcpy(live.base.name,"old-device");live.filter.magic=v==5?A1_FILTER_MAGIC:0;if(v>=5){live.filter.magic=A1_FILTER_MAGIC;live.filter.used_seconds=123;}
  memset(live.combination_buttons,0xab,sizeof(live.combination_buttons));
  if(v==6){live.automation.buttons[0][1].kind=AUTO_REBOOT;live.combination_buttons[0].kind=AUTO_FILTER_RESET;}
  if(v<3){a1_storage_v2_t old={0};old.base.magic=A1_CONFIG_MAGIC;old.base.version=v;strcpy(old.base.ssid,"preserved-wifi");strcpy(old.base.name,"old-device");memcpy(&live,&old,sizeof(old));}
  else {live.features.sleep_count=1;live.automation.tasks[0].id=42;}
  assert(migrate()==0 && writes==1 && live.base.version==A1_CONFIG_VERSION && live.filter.magic==A1_FILTER_MAGIC && live.filter.lifetime_hours==4320);
  assert(live.combination_buttons[0].kind==(v==6?AUTO_FILTER_RESET:AUTO_NONE));if(v>=5)assert(live.filter.used_seconds==123);
  assert(live.automatic.fallback_speed==30);
  assert(live.automation.buttons[0][1].kind==(v==6?AUTO_REBOOT:AUTO_FAN_MODE_TOGGLE));
  assert(!strcmp(live.base.ssid,"preserved-wifi")&&!strcmp(live.base.name,"old-device"));
  if(v>=4)assert(live.automation.tasks[0].id==42 && live.features.sleep_count==1);
 }
 /* A valid MiCO partition may still contain another firmware's app data.
  * zA1's J-Link script deliberately preserves both parameter partitions. */
 init();memset(&live,0xa5,sizeof(live));flash=live;app_storage_t unsupported=flash;
 assert(migrate()!=0 && config_recovery && writes==0 && !app_config_persist_allowed());
 assert(!memcmp(&flash,&unsupported,sizeof(flash)));
 assert(live.base.magic==A1_CONFIG_MAGIC && live.base.version==A1_CONFIG_VERSION);
 assert(!live.base.ssid[0] && !live.base.wifi_password[0] && !live.base.web_password[0]);
 assert(!live.base.fan_on && live.base.speed==60 && live.base.led_enabled);
 assert(live.features.magic==FEATURE_MAGIC && live.automation.magic==AUTOMATION_MAGIC);
 assert(live.automation.buttons[0][1].kind==AUTO_FAN_MODE_TOGGLE);
 for(unsigned v=0;v<=A1_CONFIG_VERSION+1;v+=A1_CONFIG_VERSION+1){
  init();live.base.version=v;strcpy(live.base.ssid,"keep-unsupported-config");flash=live;unsupported=flash;
  assert(migrate()!=0 && writes==0 && config_recovery && !memcmp(&flash,&unsupported,sizeof(flash)));
 }
 /* All extensions and MiCO writes share a single commit. A failed commit
  * leaves the previous Flash intact and continues in RAM-only recovery. */
 init();live.base.version=3;live.fresh_air.magic=0;live.bemfa.magic=0;live.features.magic=0;
 strcpy(live.base.ssid,"original-network");live.base.name[0]=0;flash=live;unsupported=flash;
 fail_write=sdk_write=1;
 assert(migrate()!=0 && writes==1 && config_recovery && !app_config_persist_allowed());
 assert(!memcmp(&flash,&unsupported,sizeof(flash)) && !live.base.ssid[0]);
 init();writes=0;sdk_write=1;assert(migrate()==0 && writes==1 && app_config_persist_allowed());
 init();assert(migrate()==0 && writes==0 && app_config_persist_allowed());
 puts("A1 persistence, Flash failure/retry, commands, resets, v1-v6 migration and foreign firmware startup: OK");
 /* v8 appends CO2 thresholds without moving or rewriting existing settings. */
 init();live.base.version=8;live.filter.used_seconds=789;live.automatic.fallback_speed=47;
 memset(&live.co2,0xab,sizeof(live.co2));
 app_storage_t co2_before=live;
 assert(migrate()==0 && live.base.version==A1_CONFIG_VERSION);
 assert(live.co2.on_co2==1500 && live.co2.target_co2==1000 && live.co2.low_co2==800 && live.co2.high_co2==2000);
 co2_before.base.version=A1_CONFIG_VERSION;co2_before.co2=live.co2;co2_before.filter_stock=live.filter_stock;
 assert(!memcmp(&co2_before,&live,sizeof(live)));
 live.co2.target_co2=900;assert(migrate()==0 && live.co2.target_co2==900);
 /* v7 power defaults migrate once; custom gestures and unrelated data survive. */
 init();live.base.version=7;live.automation.buttons[0][0]=(automation_action_t){.kind=AUTO_FAN,.value=2};
 live.automation.buttons[0][31]=(automation_action_t){.kind=AUTO_REBOOT};
 live.automatic.fallback_speed=47;live.filter.used_seconds=456;
 assert(migrate()==0 && live.base.version==A1_CONFIG_VERSION);
 assert(live.automation.buttons[0][0].kind==AUTO_FAN_PANEL);
 assert(live.automation.buttons[0][30].kind==AUTO_FAN && !live.automation.buttons[0][30].value);
 assert(live.automation.buttons[0][31].kind==AUTO_REBOOT);
 assert(live.automatic.fallback_speed==47 && live.filter.used_seconds==456);
 init();live.base.version=7;live.automation.buttons[0][0]=(automation_action_t){.kind=AUTO_LIGHT};
 assert(migrate()==0 && live.automation.buttons[0][0].kind==AUTO_LIGHT && live.automation.buttons[0][30].kind==AUTO_NONE);
 init();live.base.version=7;live.automation.buttons[0][0]=(automation_action_t){.kind=AUTO_FAN,.value=2,.speed=42};
 assert(migrate()==0 && live.automation.buttons[0][0].kind==AUTO_FAN && live.automation.buttons[0][0].speed==42);
 /* Exact v9 prefix must survive migration, including fresh-air, CO2 and Bemfa. */
 init();live.base.version=9;strcpy(live.base.ssid,"keep-network");
 live.fresh_air.speed=61;live.fresh_air.fallback_speed=43;strcpy(live.bemfa.app_id,"kept-id");strcpy(live.bemfa.secret_key,"kept-key");
 live.co2.target_co2=917;live.filter.used_seconds=4320*1800;
 memset(&live.filter_stock,0xa5,sizeof(live.filter_stock));
 app_storage_t v9_before=live;v9_before.base.version=10;
 assert(migrate()==0 && live.base.version==10 && !live.filter_stock.weighted);
 assert(!memcmp(&v9_before,&live,offsetof(app_storage_t,filter_stock)));
 assert(live.filter_stock.usage==6000000 && !live.filter_stock.air_litres);
 app_storage_t reboot=live;writes=0;assert(migrate()==0 && writes==0 && !memcmp(&live,&reboot,sizeof(live)));
 init();live.base.version=9;fail_write=1;assert(migrate()!=0);
 /* v9 appends stock accounting without moving old fields or resetting life. */
 init();live.base.version=9;live.filter.used_seconds=4320*1800;
 memset(&live.filter_stock,0xa5,sizeof(live.filter_stock));
 assert(migrate()==0 && live.base.version==10 && !live.filter_stock.weighted);
 assert(live.filter_stock.usage==6000000 && a1_filter_stock_remaining(&live.filter_stock)==50);
 assert(!strcmp(cmd("filter.model.set","{\"weighted\":1}"),"invalid_params"));
 assert(!strcmp(cmd("filter.model.set","{\"weighted\":true,\"extra\":1}"),"invalid_params"));
 a1_state.ota_active=true;assert(!strcmp(cmd("filter.model.set","{\"weighted\":true}"),"busy"));a1_state.ota_active=false;
 fail_write=1;assert(!strcmp(cmd("filter.model.set","{\"weighted\":true}"),"storage_error"));
 assert(!live.filter_stock.weighted);fail_write=0;
 assert(!strcmp(cmd("filter.model.set","{\"weighted\":true}"),"accepted") && flash.filter_stock.weighted);
 assert(!strcmp(cmd("filter.reset","{}"),"accepted") && !flash.filter_stock.usage && flash.filter_stock.weighted);
 a1_filter_observe(100,150);now+=60000;a1_filter_observe(0,15);
 assert(live.filter_stock.usage==975 && live.filter_stock.air_litres==6500);
 assert(a1_config_flush(false) && flash.filter_stock.usage==975);
 fail_write=1;assert(!strcmp(cmd("filter.reset","{}"),"storage_error"));
 assert(live.filter_stock.usage==975 && live.filter_stock.air_litres==6500);
 fail_write=0;
 puts("A1 stock migration/accounting/persistence/model rollback: OK");
 /* Language and device name form one persisted transaction; storage failure
  * must retain both the live language and unrelated network credentials. */
 init();strcpy(live.base.ssid,"kept-wifi");strcpy(live.base.wifi_password,"kept-password");
 strcpy(live.base.name,"kept-user-name");assert(device_language_get()==PH_LANG_ZH);
 assert(!strcmp(cmd("config.set","{\"section\":\"device\",\"values\":{\"language\":\"en\"}}"),"accepted"));
 assert(writes==1 && device_language_get()==PH_LANG_EN && flash.features.language==PH_LANG_EN);
 assert(!strcmp(live.base.name,"kept-user-name") && !strcmp(flash.base.ssid,"kept-wifi") && !strcmp(flash.base.wifi_password,"kept-password"));
 app_storage_t language_before=live;unsigned language_writes=writes;
 const char *invalid_language[]={"{\"section\":\"device\",\"values\":{\"language\":\"fr\"}}", "{\"section\":\"device\",\"values\":{\"language\":1}}", "{\"section\":\"device\",\"values\":{}}"};
 for(unsigned i=0;i<sizeof(invalid_language)/sizeof(invalid_language[0]);i++)
  assert(!strcmp(cmd("config.set",invalid_language[i]),"invalid_params"));
 assert(writes==language_writes && !memcmp(&live,&language_before,sizeof(live)));
 fail_write=1;
 assert(!strcmp(cmd("config.set","{\"section\":\"device\",\"values\":{\"language\":\"zh-CN\",\"name\":\"changed\"}}"),"storage_error"));
 assert(device_language_get()==PH_LANG_EN && !memcmp(&live.base,&language_before.base,sizeof(live.base)) && flash.features.language==PH_LANG_EN);
 fail_write=0;live=flash;assert(device_language_get()==PH_LANG_EN);
 assert(!strcmp(cmd("config.set","{\"section\":\"device\",\"values\":{\"language\":\"zh-CN\"}}"),"accepted"));
 assert(device_language_get()==PH_LANG_ZH && flash.features.language==PH_LANG_ZH && !strcmp(live.base.name,"kept-user-name"));
 puts("A1 persisted language: validation, atomic name rollback, restart and credential preservation OK");
 puts("A1 filter production persistence, Flash failure/retry, commands, resets and v1-v6 migration: OK");
}
'''.replace('PRODUCTION', prefix).replace('MIGRATION', migration).replace('WEIGHT', weight)
        json_dir = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT / 'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        with tempfile.TemporaryDirectory() as temp:
            host_json = Path(temp) / 'json'
            shutil.copytree(json_dir, host_json)
            for path in host_json.glob('*.c'):
                path.write_text(path.read_text().replace('%ld', '%d'))
            json_dir = host_json
            source = Path(temp) / 'filter.c'
            source.write_text(harness)
            binary = Path(temp) / 'filter'
            command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-ffunction-sections', '-Wl,--gc-sections', '-DFEATURE_DEVICE_A1', '-Wno-pointer-sign', '-Wno-misleading-indentation', '-I'+str(ROOT), '-I'+str(ROOT/'A1'), '-I'+str(ROOT/'tests/host'), '-I'+str(json_dir), str(source), str(ROOT/'A1/fresh_air_defaults.c'), str(ROOT/'common/mqtt/bemfa_config.c'), *map(str, json_dir.glob('*.c')), '-lm', '-o', str(binary)]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
            subprocess.run(command, check=True)
            subprocess.run([binary], check=True)
