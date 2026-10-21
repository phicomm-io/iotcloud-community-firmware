"""Execute production A1/M1 status builders with fixed hardware snapshots."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StatusViewsTest(unittest.TestCase):
    def test_full_live_and_validation(self):
        json_dir = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT / 'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        for model in ('A1', 'M1'):
            with self.subTest(model=model), tempfile.TemporaryDirectory() as temp:
                temp = Path(temp)
                host_json = temp / 'json'
                shutil.copytree(json_dir, host_json)
                for p in host_json.glob('*.c'):
                    p.write_text(p.read_text().replace('%ld', '%d'))
                source = (ROOT / model / 'http.c').read_text()
                builders = source[source.index('static void str('):source.index('static void memory_response(')]
                counters = sorted(set(re.findall(r'\bhttp_accept_\w+', builders)))
                prefix = model.lower()
                harness = r'''
#include "MODEL/MODEL_LOWER.h"
#include "common/status_view.h"
static unsigned expensive;
static MODEL_LOWER_config_t cfg;
static MODEL_LOWER_state_t snapshot;
char MODEL_LOWER_id[13]="aabbccddeeff";
uint32_t mico_rtos_get_time(void){return 25000;}
void MODEL_LOWER_config_get(MODEL_LOWER_config_t *out){*out=cfg;}
void MODEL_LOWER_state_get(MODEL_LOWER_state_t *out){*out=snapshot;}
static void phicomm_variant_json(json_object *o){expensive++;json_build_object_add(o,"full_resources",json_object_new_boolean(true));}
static void runtime_metrics_json(json_object *o){expensive++;json_build_object_add(o,"uptime_seconds",json_object_new_int(25));}
static void watchdog_status_json(json_object *o){expensive++;json_build_object_add(o,"watchdog_state",json_object_new_string("healthy"));}
#include "common/httpd/http.h"
typedef ph_http_diagnostics http_diag_t;
static http_diag_t http_diag_snapshot(void){expensive++;return (http_diag_t){0};}
#define HTTP_PENDING_CLIENTS 1
#define HTTP_TCP_CLIENT_LIMIT 4
COUNTERS
EXTRAS
BUILDERS
static void check_params(const char *s,bool valid,bool expected){
 json_object *p=json_tokener_parse(s);bool live=false;
 if(status_view_params(p,&live)!=valid){fprintf(stderr,"invalid acceptance for %s expected %d\n",s,valid);abort();}if(valid)assert(live==expected);json_object_put(p);
}
int main(void){
 check_params("{}",true,false);check_params("{\"view\":\"full\"}",true,false);check_params("{\"view\":\"live\"}",true,true);
 check_params("{\"view\":null}",false,false);check_params("{\"view\":1}",false,false);
 check_params("{\"view\":\"other\"}",false,false);check_params("{\"extra\":1}",false,false);
 check_params("{\"view\":\"live\",\"other\":true}",false,false);
 strcpy(cfg.name,"test name");strcpy(cfg.ssid,"test ssid");snapshot.wifi_connected=true;
 SETUP
 json_object *full=MODEL_LOWER_status_json();assert(full && expensive==4);
 expensive=0;json_object *live=MODEL_LOWER_status_live_json();assert(live && !expensive);
 assert(json_object_object_get(full,"ssid"));assert(!json_object_object_get(live,"ssid"));
 assert(json_object_object_get(full,"http_tcp_limit_requested"));assert(!json_object_object_get(live,"http_tcp_limit_requested"));
 json_object_object_foreach(live,key,value){
  json_object *old=json_object_object_get(full,key);
  if(!strcmp(key,"scd40")){
   json_object_object_foreach(value,ck,cv){json_object *ov=json_object_object_get(old,ck);assert(!strcmp(json_object_to_json_string(cv),json_object_to_json_string(ov)));}
  }else assert(!strcmp(json_object_to_json_string(value),json_object_to_json_string(old)));
 }
 assert(strlen(json_object_to_json_string(live))<strlen(json_object_to_json_string(full)));
 json_object_put(full);json_object_put(live);
 for(unsigned i=0;i<10000;i++){
  json_object *cycle=i%2?MODEL_LOWER_status_live_json():MODEL_LOWER_status_json();
  assert(cycle && json_object_to_json_string(cycle));json_object_put(cycle);
 }
 SENSOR_CHECK
 puts("MODEL full/live parity, skipped diagnostics and strict view validation: OK");
}
'''
                extras = '''
void feature_effective_fan(bool on,unsigned speed,bool *eo,unsigned *es){*eo=on;*es=speed;}
bool feature_fan_auto(void){return false;}
unsigned feature_fan_limit(void){return 100;}
json_object *a1_filter_json(void){return json_object_new_object();}
''' if model == 'A1' else '''
void m1_ntp_host_get(char out[64]){strcpy(out,"pool.ntp.org");}
bool m1_sample_fresh(const m1_sample_t *s,uint32_t now){return s->received && now-s->updated_ms<20000;}
'''
                setup = 'snapshot.scd40.ready=true;snapshot.scd40.received=true;snapshot.scd40.updated_ms=24000;snapshot.scd40.co2_ppm=900;' if model == 'M1' else ''
                sensor_check = '''
 snapshot.scd40.updated_ms=0;live=m1_status_live_json();
 json_object *co2=json_object_object_get(live,"scd40");
 assert(!json_object_get_boolean(json_object_object_get(co2,"valid")));
 assert(!json_object_object_get(co2,"co2_ppm"));
 assert(!json_object_object_get(live,"temperature"));json_object_put(live);
''' if model == 'M1' else ''
                for key, value in [('MODEL_LOWER', prefix), ('MODEL', model), ('COUNTERS', ''.join('static int '+c+';\n' for c in counters)), ('EXTRAS', extras), ('BUILDERS', builders), ('SETUP', setup), ('SENSOR_CHECK', sensor_check)]:
                    harness = harness.replace(key, value)
                probe = temp / 'probe.c'
                probe.write_text(harness)
                cmd = [os.environ.get('CC', 'cc'), '-std=gnu99', '-I'+str(ROOT), '-I'+str(ROOT / model), '-I'+str(ROOT / 'tests/host'), '-I'+str(host_json), str(probe), *map(str, host_json.glob('*.c')), '-o', str(temp / 'probe')]
                if model == 'A1':
                    cmd.append('-DFEATURE_DEVICE_A1')
                if os.environ.get('SANITIZE') == '1':
                    cmd += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
                subprocess.run(cmd, check=True)
                subprocess.run([temp / 'probe'], check=True)
