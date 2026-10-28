"""Execute real A1/M1 capture and native pure encoder across state changes."""
import os
from pathlib import Path
import subprocess,tempfile,unittest
from tests.test_http_json_budget import function
ROOT=Path(__file__).resolve().parents[1]
COMMON=r'''
#define PHICOMM_HOST_CUSTOM_MEMORY 1
#include "MODEL/LOWER.h"
#include "common/device_status.h"
#include <math.h>
static unsigned expensive,held,blocks;static bool allow_getters=true,inject_update;
static uint32_t now=25000;static LOWER_config_t cfg;static LOWER_state_t state;
char LOWER_id[13]="aabbccddeeff";
static void getter(void){assert(allow_getters);}
uint32_t mico_rtos_get_time(void){getter();return now;}
typedef struct {int num_of_chunks,total_memory,allocted_memory,free_memory;}micoMemInfo_t;
static micoMemInfo_t memory={3,60000,30000,30000};
static const micoMemInfo_t*MicoGetMemoryInfo(void){getter();expensive++;return &memory;}
#include "common/runtime_metrics.h"
void watchdog_status_capture(watchdog_status_t*out){getter();expensive++;*out=(watchdog_status_t){true,false,true,0,"","unknown"};}
ph_http_diagnostics ph_http_snapshot(void){getter();expensive++;return (ph_http_diagnostics){0};}
void *ph_wire_malloc(size_t n){assert(!held);void*p=malloc(n);if(p)blocks++;return p;}
void ph_wire_free(void*p){if(p){assert(blocks);blocks--;free(p);}}
MODEL_SETUP
static int test_lock(mico_mutex_t*m){getter();ON_WAIT assert(!*m);*m=1;held++;return 0;}
static int test_unlock(mico_mutex_t*m){assert(*m&&held);*m=0;held--;return 0;}
#define mico_rtos_lock_mutex test_lock
#define mico_rtos_unlock_mutex test_unlock
#define LOWER_state state
CAPTURE
static ph_buf*encode(const device_status_t*s,const device_status_full_t*f){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));device_status_mp(&w,s,f);ph_buf*b=phw_take(&w);assert(b);return b;}
int main(void){
 strcpy(cfg.name,"test name");strcpy(cfg.ssid,"test ssid");strcpy(cfg.web_password,"secret");state.wifi_connected=true;
 device_status_t frozen;device_status_full_t full;LOWER_status_capture(&frozen,&full);assert(expensive&&full.network.web_password_set&&!held);expensive=0;
 device_status_t live;LOWER_status_capture(&live,NULL);assert(!expensive&&!held);
 allow_getters=false;ph_buf*wire=encode(&frozen,&full);assert(!strstr(wire->buf,"secret"));
 memset(&cfg,0xa5,sizeof(cfg));memset(&state,0xa5,sizeof(state));now=999999;memory.free_memory=1;
 ph_buf*again=encode(&frozen,&full);assert(wire->len==again->len&&!memcmp(wire->buf,again->buf,wire->len));ph_buf_free(wire);ph_buf_free(again);
 for(unsigned i=0;i<10000;i++){wire=encode(&frozen,&full);ph_buf_free(wire);}assert(!blocks);
 allow_getters=true;memset(&cfg,0,sizeof(cfg));memset(&state,0,sizeof(state));now=25000;
 SENSOR_CHECK
 assert(!held&&!blocks);puts("MODEL capture purity/lock race/10000 encode cycles/sensor validity PASS");return 0;
}
'''
A1=r'''
mico_mutex_t a1_config_lock,a1_lock;
#define a1_config (&cfg)
void feature_values_snapshot(feature_values_t*out){getter();*out=(feature_values_t){.value={23,45,123,.1},.co2=1400,.sample_count=2,.co2_sample_count=1,.configured_count=2,.statistic=1,.valid=true};}
void feature_fan_snapshot(bool*on,unsigned*speed,bool*automatic,unsigned*limit){getter();*on=cfg.fan_on;*speed=cfg.speed;*automatic=false;*limit=100;}
void a1_filter_get(a1_filter_storage_t*out){getter();expensive++;*out=(a1_filter_storage_t){0,10,2000};}
void a1_filter_stock_get(a1_filter_stock_t*out){getter();expensive++;a1_filter_stock_defaults(out,false);}
'''
M1=r'''
mico_mutex_t config_lock,m1_lock;
typedef struct{m1_config_t base;struct{char ntp_host[64];}features;}app_storage_t;
static app_storage_t storage;
#define app_config (&storage.base)
#undef cfg
#define cfg storage.base
void m1_co2_capture(device_m1_co2_t*out){getter();assert(!held);*out=(device_m1_co2_t){.revision=3,.interval=5,.low=1000,.high=1500,.mode=1,.supported=true,.present=true,.valid=true,.showing=true,.ppm=1234};}
'''
M1_RACE='if(inject_update){state.sample.received=true;state.sample.updated_ms=state.scd40.updated_ms=25005;state.scd40.ready=state.scd40.received=true;state.barometer=(barometer_state_t){.ready=true,.received=true,.updated_ms=25005,.chip_id=0x50,.address=0x77,.pressure_centi_pa=10132400};now=25010;inject_update=false;}'
M1_CHECK='\n inject_update=true;m1_status_capture(&frozen,&full);assert(frozen.now_ms==25010&&frozen.sensor.m1.sample.updated_ms==25005);wire=encode(&frozen,&full);\n ph_value root,b,v;int64_t age;double pressure;bool valid;assert(phv_parse(1,wire->buf,wire->len,&root));\n assert(phv_get(&root,"sensor_valid",&v)&&phv_bool(&v,&valid)&&valid);\n assert(phv_get(&root,"scd40",&b)&&phv_get(&b,"age_ms",&v)&&phv_int(&v,&age)&&age==5);assert(phv_get(&root,"barometer",&b)&&phv_get(&b,"pressure_hpa",&v)&&phv_number(&v,&pressure)&&fabs(pressure-1013.24)<1e-9);ph_buf_free(wire);\n state.barometer.updated_ms=15010;m1_status_capture(&frozen,&full);wire=encode(&frozen,&full);assert(phv_parse(1,wire->buf,wire->len,&root));assert(phv_get(&root,"barometer",&b)&&phv_get(&b,"valid",&v)&&phv_bool(&v,&valid)&&!valid);assert(phv_get(&b,"pressure_hpa",&v)&&v.kind==PHV_NULL);ph_buf_free(wire);\n'
A1_CHECK=r'''
 a1_status_capture(&frozen,&full);assert(frozen.sensor.a1.sample_count==2&&frozen.sensor.a1.co2==1400);const float missing[]={NAN,INFINITY,-INFINITY};
 for(unsigned n=0;n<3;n++){frozen.sensor.a1.value[3]=missing[n];wire=encode(&frozen,&full);ph_value root,b,v;assert(phv_parse(1,wire->buf,wire->len,&root));assert(phv_get(&root,"sensors",&b)&&phv_get(&b,"formaldehyde",&v)&&v.kind==PHV_NULL);ph_buf_free(wire);}
'''
class StatusViewsTest(unittest.TestCase):
 def test_capture_is_owned_pure_and_race_safe(self):
  for model in ('A1','M1'):
   with self.subTest(model=model),tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp);prefix=model.lower();capture=function((ROOT/model/'main.c').read_text(),f'void {prefix}_status_capture(')
    text=COMMON
    for key,value in [('MODEL_SETUP',A1 if model=='A1' else M1),('ON_WAIT','' if model=='A1' else M1_RACE),('CAPTURE',capture),('SENSOR_CHECK',A1_CHECK if model=='A1' else M1_CHECK),('LOWER',prefix),('MODEL',model)]:text=text.replace(key,value)
    src=d/'case.c';src.write_text(text)
    cmd=[os.environ.get('CC','cc'),'-std=gnu99','-DPHICOMM_APPLICATION_WATCHDOG=1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),'-I'+str(ROOT/'tests/host'),str(src),*[str(ROOT/p) for p in ('common/device_status.c','common/typed_wire.c','common/wire_buffer.c')],'-lm','-o',str(d/'case')]
    result=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    result=subprocess.run([d/'case'],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout)
if __name__=='__main__':unittest.main()
