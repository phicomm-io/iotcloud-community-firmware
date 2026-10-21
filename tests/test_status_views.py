"""Actual model capture + common pure encoder, against frozen production schema."""
import os
from pathlib import Path
import subprocess, tempfile, unittest
from tests.sdk_json import copy_patched_json
from tests.test_http_json_budget import function
ROOT=Path(__file__).resolve().parents[1]
class StatusViewsTest(unittest.TestCase):
 def test_full_live_and_validation(self):
  sdk=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
  for model in ('A1','M1'):
   with self.subTest(model=model), tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp);json=copy_patched_json(sdk,d/'sdk')
    for p in json.glob('*.c'):p.write_text(p.read_text().replace('%ld','%d'))
    prefix=model.lower()
    capture=function((ROOT/model/'main.c').read_text(),f'void {prefix}_status_capture(')
    source=(ROOT/model/'http.c').read_text();builders=source[source.index('static __attribute__((noinline)) json_object *'+prefix+'_status_full('):source.index('static void memory_response(')]
    harness=r'''
#include "MODEL/LOWER.h"
#include "common/device_status.h"
#include "common/status_view.h"
#include "json_heap.h"
static unsigned expensive,held,fail_after,permits;
static bool allow_getters=true,inject_update;
static uint32_t now=25000;
static LOWER_config_t cfg;
static LOWER_state_t state;
char LOWER_id[13]="aabbccddeeff";
static void getter(void){assert(allow_getters);}
uint32_t mico_rtos_get_time(void){getter();return now;}
void LOWER_config_get(LOWER_config_t*out);
void LOWER_state_get(LOWER_state_t*out){getter();*out=state;}
typedef struct {int num_of_chunks,total_memory,allocted_memory,free_memory;} micoMemInfo_t;
static micoMemInfo_t memory={3,60000,30000,30000};
static const micoMemInfo_t *MicoGetMemoryInfo(void){getter();expensive++;return &memory;}
#include "common/runtime_metrics.h"
#include "common/firmware_variant.h"
void watchdog_status_capture(watchdog_status_t*out){getter();expensive++;*out=(watchdog_status_t){true,false,true,0,"","unknown"};}
void watchdog_status_encode(json_object*o,const watchdog_status_t*s){json_build_object_add(o,"watchdog_enabled",json_object_new_boolean(s->enabled));}
void watchdog_status_json(json_object*o){watchdog_status_t s;watchdog_status_capture(&s);watchdog_status_encode(o,&s);}
static void runtime_metrics_json(json_object*o){device_runtime_t s;runtime_metrics_capture(&s,mico_rtos_get_time());device_runtime_encode(o,&s);}
static ph_http_diagnostics http_diag_snapshot(void){getter();expensive++;return (ph_http_diagnostics){0};}
#define ph_http_snapshot http_diag_snapshot
typedef ph_http_diagnostics http_diag_t;
#define HTTP_PENDING_CLIENTS 1
#define HTTP_TCP_CLIENT_LIMIT CAP
#define LOWER_state state
MODEL_SETUP
void LOWER_config_get(LOWER_config_t*out){getter();*out=cfg;}
static int test_lock(mico_mutex_t*m){getter();ON_WAIT assert(!*m);*m=1;held++;return 0;}
static int test_unlock(mico_mutex_t*m){assert(*m&&held);*m=0;held--;return 0;}
#define mico_rtos_lock_mutex test_lock
#define mico_rtos_unlock_mutex test_unlock
CAPTURE
#include "common/device_status.c"
#include "tests/http_core/legacy_LOWER_status.c"
BUILDERS
int json_heap_permit(size_t n){(void)n;assert(!held);permits++;return !fail_after||permits!=fail_after;}
static void check_params(const char*s,bool valid,bool wanted){json_object*p=json_tokener_parse(s);bool live=false;assert(status_view_params(p,&live)==valid);if(valid)assert(live==wanted);json_object_put(p);}
static void empty(void){json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks);}
int main(void){
 check_params("{}",true,false);check_params("{\"view\":\"full\"}",true,false);check_params("{\"view\":\"live\"}",true,true);
 check_params("{\"view\":null}",false,false);check_params("{\"view\":1}",false,false);check_params("{\"view\":\"other\"}",false,false);check_params("{\"extra\":1}",false,false);check_params("{\"view\":\"live\",\"other\":true}",false,false);
 strcpy(cfg.name,"test name");strcpy(cfg.ssid,"test ssid");strcpy(cfg.web_password,"secret");state.wifi_connected=true;
 SETUP
 json_object*full=LOWER_status_json();assert(full&&expensive);expensive=0;
 json_object*live=LOWER_status_live_json();assert(live&&!expensive);
 json_object*old=legacy_LOWER_status_json();assert(old);assert(!strcmp(json_object_to_json_string(full),json_object_to_json_string(old)));json_object_put(old);
 old=legacy_LOWER_status_live_json();assert(old);assert(!strcmp(json_object_to_json_string(live),json_object_to_json_string(old)));json_object_put(old);
 assert(json_object_object_get(full,"ssid"));assert(!json_object_object_get(live,"ssid"));
 assert(!strstr(json_object_to_json_string(full),"secret"));assert(strlen(json_object_to_json_string(live))<strlen(json_object_to_json_string(full)));
 json_object_object_foreach(live,key,value){json_object*prev=json_object_object_get(full,key);if(!strcmp(key,"scd40")){json_object_object_foreach(value,ck,cv){json_object*ov=json_object_object_get(prev,ck);assert(!strcmp(json_object_to_json_string(cv),json_object_to_json_string(ov)));}}else assert(!strcmp(json_object_to_json_string(value),json_object_to_json_string(prev)));}
 json_object_put(live);json_object_put(full);empty();
 device_status_t frozen;device_status_full_t all;
 LOWER_status_capture(&frozen,&all);allow_getters=false;
 full=device_status_encode(&frozen,&all);assert(full);char*saved=strdup(json_object_to_json_string(full));json_object_put(full);
 memset(&cfg,0xa5,sizeof(cfg));memset(&state,0xa5,sizeof(state));now=999999;memory.free_memory=1;
 full=device_status_encode(&frozen,&all);assert(full&&!strcmp(saved,json_object_to_json_string(full)));free(saved);json_object_put(full);empty();
 for(unsigned i=1;i<=200;i++){permits=0;fail_after=i;full=device_status_encode(&frozen,&all);if(full)json_object_put(full);empty();}fail_after=0;
 for(unsigned i=0;i<10000;i++){full=device_status_encode(&frozen,i%2?&all:NULL);assert(full&&json_object_to_json_string(full));json_object_put(full);}empty();
 allow_getters=true;memset(&cfg,0,sizeof(cfg));memset(&state,0,sizeof(state));SENSOR_CHECK
 printf("MODEL frozen schema parity, no getters/locks in encode, OOM/10000 cycles, live skips diagnostics: PASS (%zu/%zu bytes)\n",sizeof(device_status_t),sizeof(device_status_full_t));
}
'''
    if model=='A1':
     extra=r'''
mico_mutex_t a1_config_lock,a1_lock;
#define a1_config (&cfg)
void feature_fan_snapshot(bool*on,unsigned*speed,bool*automatic,unsigned*limit){getter();*on=cfg.fan_on;*speed=cfg.speed;*automatic=false;*limit=100;}
void feature_effective_fan(bool on,unsigned speed,bool*eo,unsigned*es){getter();*eo=on;*es=speed;}
bool feature_fan_auto(void){getter();return false;}
unsigned feature_fan_limit(void){getter();return 100;}
void a1_filter_get(a1_filter_storage_t*out){getter();expensive++;*out=(a1_filter_storage_t){0,10,2000};}
json_object*a1_filter_json(void){a1_filter_storage_t f;a1_filter_get(&f);return device_filter_encode(&f);}
'''
     # The pure helper is declared below the capture and before this oracle wrapper.
     extra=extra.replace('json_object*a1_filter_json(void){a1_filter_storage_t f;a1_filter_get(&f);return device_filter_encode(&f);}','json_object*a1_filter_json(void);')
     builders='json_object*a1_filter_json(void){a1_filter_storage_t f;a1_filter_get(&f);return device_filter_encode(&f);}\n'+builders
     setup=onwait=sensor=''
    else:
     extra=r'''
mico_mutex_t config_lock,m1_lock;
typedef struct{m1_config_t base;struct{char ntp_host[64];}features;}app_storage_t;
static app_storage_t storage;
#define app_config (&storage.base)
#undef cfg
#define cfg storage.base
void m1_ntp_host_get(char out[64]){getter();memcpy(out,storage.features.ntp_host,64);}
bool m1_sample_fresh(const m1_sample_t*s,uint32_t at){getter();return s->received&&(uint32_t)(at-s->updated_ms)<M1_STALE_MS;}
'''
     onwait='if(inject_update){state.sample.received=true;state.sample.updated_ms=state.scd40.updated_ms=25005;state.scd40.ready=state.scd40.received=true;now=25010;inject_update=false;}'
     setup='state.sample.received=true;state.sample.updated_ms=24000;state.scd40.ready=state.scd40.received=true;state.scd40.updated_ms=24000;state.scd40.co2_ppm=900;'
     sensor=r'''now=24000;inject_update=true;m1_status_capture(&frozen,NULL);assert(frozen.now_ms==25010);live=device_status_encode(&frozen,NULL);assert(json_object_get_boolean(json_object_object_get(live,"sensor_valid")));json_object*co2=json_object_object_get(live,"scd40");assert(json_object_get_boolean(json_object_object_get(co2,"valid")));assert(json_object_get_int(json_object_object_get(co2,"age_ms"))==5);assert(!json_object_object_get(co2,"temperature"));assert(!json_object_object_get(co2,"humidity"));json_object_put(live);state.scd40.updated_ms=0;live=m1_status_live_json();co2=json_object_object_get(live,"scd40");assert(!json_object_get_boolean(json_object_object_get(co2,"valid")));assert(!json_object_object_get(co2,"co2_ppm"));json_object_put(live);empty();'''
    for k,v in [('MODEL_SETUP',extra),('ON_WAIT',onwait),('CAPTURE',capture),('BUILDERS',builders),('SETUP',setup),('SENSOR_CHECK',sensor),('CAP','3' if model=='A1' else '4'),('LOWER',prefix),('MODEL',model)]:harness=harness.replace(k,v)
    src=d/'test.c';src.write_text(harness)
    cmd=[os.environ.get('CC','cc'),'-std=gnu99','-DPHICOMM_APPLICATION_WATCHDOG=1','-I'+str(ROOT),'-I'+str(ROOT/'tests/host'),'-I'+str(json),str(src),*map(str,json.glob('*.c')),'-o',str(d/'test')]
    if os.environ.get('SANITIZE')=='1':cmd+=['-g','-fno-omit-frame-pointer','-fsanitize=address,undefined','-fno-pie','-no-pie']
    subprocess.run(cmd,check=True);subprocess.run([d/'test'],check=True)
if __name__=='__main__':unittest.main()
