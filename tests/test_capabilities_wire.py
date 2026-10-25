"""Actual capability graph/command/HTTP wire parity and fail-closed allocation.

Only OTA worker I/O and unrelated product actions are replaced. The capability
catalog/builders, dispatcher, validators, scopes and HTTP response code are real.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json
from tests.test_http_json_budget import function

ROOT = Path(__file__).resolve().parents[1]


def ota_catalog(mode):
    if mode == 1:
        source = (ROOT/'common/ota/device_update.inc').read_text()
    else:
        source = (ROOT/'common/ota/update.inc').read_text()
        if mode == 0:
            source = source[source.index('#else\nstatic const char *const update_capability_names'):]
    start = source.index('static const char *const update_capability_names')
    end = source.index(';', start)+1
    return source[start:end]+'\n'+function(source, 'static void update_capabilities(')


def service_source(mode):
    source = (ROOT/'common/service.c').read_text()
    shim = ota_catalog(mode)+r'''
static json_object *update_command(const char*a,json_object*p,const char**c,bool*h){
 (void)a;(void)p;(void)c;*h=false;return NULL;
}
'''
    return source.replace('#include "ota/update.inc"', shim)


def product_source():
    source = (ROOT/'common/service_product.inc').read_text()
    start = source.index('static const char *const svc_capability_actions')
    end = source.index('static __attribute__((noinline)) json_object *service_product_inner(')
    return function(source, 'static void svc_string(')+'\n'+source[start:end]


FIXTURE = r'''
#include "mico.h"
#include "common/service.h"
#include "common/status_view.h"
#include "common/firmware_variant.h"
#include "common/httpd/product.h"
#include "common/json_owned.h"
#include "json_heap.h"
#include "printbuf.h"
uint32_t test_now;
static bool recovery,active,flip_recovery;
static unsigned recovery_reads,attempts,fault_at,mutate_at;
static const char **mutate_names;
static const char *mutate_value;
int capability_test_permit(size_t bytes){
 unsigned nth=++attempts;
 if(mutate_names && nth==mutate_at)mutate_names[0]=mutate_value;
 return json_heap_permit(bytes)&&(!fault_at||nth!=fault_at);
}
bool device_config_recovery(void){bool value=recovery;recovery_reads++;if(flip_recovery)recovery=!recovery;return value;}
void device_config_recovery_set(bool value){recovery=value;}
#ifdef PHICOMM_DEVICE_OTA
void ota_job_init(void){}
bool ota_job_active(void){return active;}
bool ota_job_cancel(void){return false;}
json_object *ota_job_status(void){abort();return NULL;}
#endif
PRODUCT_CAPABILITIES
json_object *service_product(const char*a,json_object*p,const char**c){(void)a;(void)p;*c="unsupported";return NULL;}
json_object *automation_command(const char*a,json_object*p,const char**c,bool*h){(void)a;(void)p;(void)c;*h=false;return NULL;}
struct printbuf *service_product_status_wire(bool l,bool e,const char*i){(void)l;(void)e;(void)i;abort();return NULL;}
static void empty(void){json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks);}
static void save(char *out,size_t size,struct printbuf *wire){assert(wire&&wire->bpos==strlen(wire->buf)&&wire->bpos<size);strcpy(out,wire->buf);printbuf_free(wire);}
static json_object *request(const char *params,const char *id){
 char text[700];snprintf(text,sizeof(text),"{\"action\":\"capabilities.get\",\"params\":%s%s}",params,id?id:"");return service_parse(text,strlen(text));
}
static void parity(void){
 unsigned cases=0;
 const char *params[]={"{}","{\"view\":\"full\"}","{\"view\":\"live\"}","{\"ignored\":[1,null,true,{\"x\":2}]}"};
 const char *ids[]={NULL,",\"request_id\":\"read-123\"",",\"request_id\":\"0123456789012345678901234567890123456789012345678901234567890123\""};
 for(unsigned r=0;r<2;r++)for(unsigned a=0;a<ACTIVE_MODES;a++)for(unsigned p=0;p<4;p++)for(unsigned i=0;i<3;i++){
  recovery=r;active=a;
  json_object *q=request(params[p],ids[i]);assert(q);
  char expected[4096];save(expected,sizeof(expected),json_owned_to_wire(service_command(q)));
  recovery_reads=0;struct printbuf *wire=service_command_wire(q);assert(wire&&!strcmp(expected,wire->buf));
  assert(recovery_reads==(active?0:1));printbuf_free(wire);
  wire=service_mqtt_command_wire(q);assert(wire&&!strcmp(expected,wire->buf));printbuf_free(wire);json_object_put(q);empty();cases++;
 }
 for(unsigned r=0;r<2;r++)for(unsigned a=0;a<ACTIVE_MODES;a++){
  recovery=r;active=a;char expected[4096];save(expected,sizeof(expected),json_owned_to_wire(service_query("capabilities.get")));
  recovery_reads=0;struct printbuf *wire=service_query_wire("capabilities.get");assert(wire&&!strcmp(expected,wire->buf));assert(recovery_reads==(active?0:1));
  json_object *out=json_tokener_parse(wire->buf),*data=json_object_object_get(out,"data");assert(out&&data);
  assert(json_object_is_type(json_object_object_get(data,"api_version"),json_type_int));
  assert(json_object_is_type(json_object_object_get(data,"full_resources"),json_type_boolean));
  json_object *actions=json_object_object_get(data,"actions");assert(json_object_is_type(actions,json_type_array));
  if(active){assert(!json_object_object_get(data,"model"));assert(!json_object_object_get(data,"config_recovery"));assert(json_object_array_length(actions)==6);}
  else{assert(!strcmp(json_object_get_string(json_object_object_get(data,"model")),DEVICE_MODEL));assert(json_object_is_type(json_object_object_get(data,"config_backup"),json_type_int));assert(json_object_get_boolean(json_object_object_get(data,"config_recovery"))==r);}
  printbuf_free(wire);json_object_put(out);empty();cases++;
 }
 printf("%s variant=%s OTA=%u: %u actual graph/query/command/MQTT bytes/order/types/recovery cases PASS\n",DEVICE_MODEL,PHICOMM_FIRMWARE_VARIANT,OTA_MODE,cases);
}
static void invalid_requests(void){
 const char *inputs[]={
 "null","[]","{}","{\"action\":\"capabilities.get\",\"params\":null}",
 "{\"action\":\"capabilities.get\",\"params\":[]}",
 "{\"action\":\"capabilities.get\",\"params\":{},\"extra\":true}",
 "{\"action\":\"capabilities.get\",\"params\":{},\"request_id\":3}",
 "{\"action\":\"capabilities.get\",\"params\":{},\"request_id\":\"bad/id\"}",
 "{\"action\":\"capabilities.get\",\"params\":{},\"request_id\":\"\"}",
 "{\"action\":\"capabilities.get\",\"params\":{},\"request_id\":\"01234567890123456789012345678901234567890123456789012345678901234\"}"};
 for(unsigned a=0;a<ACTIVE_MODES;a++)for(unsigned i=0;i<sizeof(inputs)/sizeof(*inputs);i++){
  active=a;json_object *q=service_parse(inputs[i],strlen(inputs[i]));char expected[4096];
  save(expected,sizeof(expected),json_owned_to_wire(service_command(q)));assert(strstr(expected,"invalid_params"));
  struct printbuf *wire=service_command_wire(q);assert(wire&&!strcmp(expected,wire->buf));printbuf_free(wire);json_object_put(q);empty();
 }
}
static void allocation_failures(void){
 active=false;flip_recovery=false;
 attempts=0;ph_http_product p={0};ph_http_exchange x={0};p.failures=service_memory_failures();ph_http_query(&p,&x,"capabilities.get");
 assert(x.response.status==200&&p.wire&&attempts==2);ph_http_product_release(&p);empty();
 for(unsigned nth=1;nth<=2;nth++){
  attempts=0;fault_at=nth;ph_http_product_init(&p,&x);p.failures=service_memory_failures();ph_http_query(&p,&x,"capabilities.get");fault_at=0;
  assert(x.response.status==503&&!p.wire&&attempts==nth);ph_http_product_release(&p);empty();
  ph_http_product_init(&p,&x);p.failures=service_memory_failures();ph_http_query(&p,&x,"capabilities.get");assert(x.response.status==200&&p.wire);ph_http_product_release(&p);empty();
  json_object *q=request("{}",NULL);assert(q);attempts=0;fault_at=nth;struct printbuf *wire=service_mqtt_command_wire(q);fault_at=0;assert(!wire&&attempts==nth);json_object_put(q);empty();
 }
 puts("Actual HTTP/MQTT wire: exactly 2 guarded allocations, each refusal fails closed/recovery/leaks PASS");
}
static void snapshot_and_bounds(void){
 active=false;recovery=true;flip_recovery=true;recovery_reads=0;
 struct printbuf *wire=service_query_wire("capabilities.get");assert(wire&&recovery_reads==1&&!recovery&&strstr(wire->buf,"\"config_recovery\": true"));printbuf_free(wire);flip_recovery=false;empty();
 const char *extra[]={"short",NULL};
 for(unsigned shrink=0;shrink<2;shrink++){
  extra[0]=shrink?"a-long-capability":"short";mutate_names=extra;mutate_value=shrink?"x":"a-much-longer-capability";mutate_at=2;attempts=0;
  wire=service_product_capabilities_wire(NULL,extra,false);mutate_names=NULL;assert(!wire);empty();
 }
 status_sink sink={0};assert(svc_capabilities_visit(&sink,NULL,NULL,false,false));size_t size=sink.used;assert(size>100);
 char *body=malloc(size+2);memset(body,'X',size+2);memset(&sink,0,sizeof(sink));sink.buf=body;sink.capacity=size-1;
 assert(!svc_capabilities_visit(&sink,NULL,NULL,false,false)&&sink.failed);assert(body[size-1]=='X'&&body[size]=='X'&&body[size+1]=='X');free(body);empty();
 puts("Single recovery snapshot; both length changes and bounded write fail closed without overwrite PASS");
}
int main(void){service_init();parity();invalid_requests();allocation_failures();snapshot_and_bounds();return 0;}
'''


class CapabilitiesWire(unittest.TestCase):
    def test_missing_encoder_retains_actual_tc1_and_host_graph_fallback(self):
        sdk = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        sha = ROOT/'build/a1-sdk/mico-os/MiCO/security/SHAUtils'
        tc1 = (ROOT/'TC1/service.c').read_text()
        fixture = r'''
#include "mico.h"
#include "common/service.c"
#include "common/status_view.h"
uint32_t test_now;
#ifdef REAL_TC1
static struct {int mqtt_topic_mode;} cfg,*user_config=&cfg;
TC1_CAPABILITIES
json_object *service_product(const char*a,json_object*p,const char**c){(void)a;(void)p;*c="unsupported";return NULL;}
json_object *automation_command(const char*a,json_object*p,const char**c,bool*h){(void)a;(void)p;(void)c;*h=false;return NULL;}
struct printbuf *service_product_status_wire(bool l,bool e,const char*i){(void)l;(void)e;(void)i;abort();return NULL;}
#else
void m1_set_brightness(unsigned n){(void)n;abort();}
void a1_control_local(const char*a,json_object*p){(void)a;(void)p;abort();}
void UserRelaySet(int i,bool on){(void)i;(void)on;abort();}
#include "tests/host/service_adapter.inc"
#endif
static void check(void){
 struct printbuf *old=json_owned_to_wire(service_query("capabilities.get"));assert(old);
 struct printbuf *wire=service_query_wire("capabilities.get");assert(wire&&!strcmp(old->buf,wire->buf));printbuf_free(old);printbuf_free(wire);
 const char *text="{\"action\":\"capabilities.get\",\"params\":{\"ignored\":true},\"request_id\":\"read-id\"}";
 json_object *q=service_parse(text,strlen(text));assert(q);old=json_owned_to_wire(service_command(q));assert(old);
 wire=service_command_wire(q);assert(wire&&!strcmp(old->buf,wire->buf));printbuf_free(wire);
 wire=service_mqtt_command_wire(q);assert(wire&&!strcmp(old->buf,wire->buf));printbuf_free(wire);printbuf_free(old);json_object_put(q);
 json_heap_stats_t stats;json_heap_snapshot(&stats);assert(!stats.live_blocks);
}
int main(void){service_init();assert(!service_product_capabilities_wire);check();
#ifdef REAL_TC1
 cfg.mqtt_topic_mode=1;check();
#endif
 puts("Absent capability encoder: actual TC1/HOST graph query/command/MQTT bytes preserved PASS");return 0;}
'''
        fixture = fixture.replace('TC1_CAPABILITIES', function(tc1,'static void text(')+'\n'+function(tc1,'json_object *service_product_capabilities('))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            json = copy_patched_json(sdk,directory/'sdk')
            for source in json.glob('*.c'):
                source.write_text(source.read_text().replace('%ld','%d'))
            source = directory/'fallback.c'
            source.write_text(fixture)
            for model in ('REAL_TC1','HOST_TC1','HOST_A1','HOST_M1'):
                with self.subTest(model=model):
                    binary = directory/model
                    subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-D'+model,
                        '-I'+str(ROOT),'-I'+str(ROOT/'tests/stubs'),'-I'+str(sha),'-I'+str(json),
                        str(source),str(ROOT/'common/json_owned.c'),str(sha/'sha224-256.c'),
                        *map(str,json.glob('*.c')),'-o',str(binary)],check=True,timeout=60)
                    subprocess.run([str(binary)],check=True,timeout=20)

    def test_actual_dispatch_parity_all_variants_and_ota_modes(self):
        sdk = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        sha = ROOT/'build/a1-sdk/mico-os/MiCO/security/SHAUtils'
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            json = copy_patched_json(sdk, directory/'sdk')
            for source in json.glob('*.c'):
                source.write_text(source.read_text().replace('%ld','%d'))
            heap = json/'json_heap.c'
            heap.write_text('#include <stddef.h>\nint capability_test_permit(size_t);\n'+heap.read_text().replace('if (!json_heap_permit(', 'if (!capability_test_permit('))
            common = directory/'common'
            common.mkdir()
            for mode in (0,1,2):
                (common/('service'+str(mode)+'.c')).write_text(service_source(mode))
            fixture = directory/'fixture.c'
            fixture.write_text(FIXTURE.replace('PRODUCT_CAPABILITIES',product_source()))
            compiler = os.environ.get('CC','cc')
            flags = ['-std=gnu99','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                     '-I'+str(ROOT),'-I'+str(ROOT/'common'),'-I'+str(ROOT/'tests/stubs'),'-I'+str(sha),'-I'+str(json)]
            objects = []
            for source in [*json.glob('*.c'),sha/'sha224-256.c',ROOT/'common/json_owned.c',ROOT/'common/httpd/product.c']:
                target=directory/(source.stem+'.o')
                subprocess.run([compiler,*flags,'-c',str(source),'-o',str(target)],check=True,timeout=60)
                objects.append(str(target))
            for model in ('A1','M1'):
                variants = [(full,fresh,bemfa) for full in (0,1) for fresh in (0,1) for bemfa in (0,1)] if model=='A1' else [(0,0,0),(1,0,0)]
                for full,fresh,bemfa in variants:
                    for mode in (0,1,2):
                        with self.subTest(model=model,full=full,fresh=fresh,bemfa=bemfa,ota=mode):
                            definitions = ['-DDEVICE_MODEL="'+model.lower()+'"','-DOTAMODE='+str(mode),'-DOTA_MODE='+str(mode),'-DACTIVE_MODES='+str(2 if mode==1 else 1),'-DPHICOMM_FULL_RESOURCES='+str(full)]
                            if model=='A1':definitions+=['-DFEATURE_DEVICE_A1=1']
                            if fresh:definitions+=['-DA1_FRESH_AIR=1']
                            if bemfa:definitions+=['-DPHICOMM_BEMFA=1']
                            if mode==1:definitions+=['-DPHICOMM_DEVICE_OTA=1']
                            if mode==2:definitions+=['-DPHICOMM_OTA_CN_BASE_URL="http://offline.invalid"']
                            binary=directory/(model+str(full)+str(fresh)+str(bemfa)+str(mode))
                            subprocess.run([compiler,*flags,*definitions,str(common/('service'+str(mode)+'.c')),str(fixture),*objects,'-o',str(binary)],check=True,timeout=60)
                            subprocess.run([str(binary)],check=True,timeout=20)


if __name__ == '__main__':
    unittest.main()
