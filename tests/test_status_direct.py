"""Frozen pre-change oracle vs actual direct writer on three pinned ARM SDKs."""
import re
import unittest
from tests.test_http_json_budget import ROOT, status_fixture, function
from tests import test_http_json_budget as budget


def fixture(dual=True):
    text=status_fixture(dual);text=text[:text.index('int main(void){')]
    old=(ROOT/'tests/http_core/status_reference.c').read_text()
    names=re.findall(r'(?:void|json_object\s*\*|char\s*\*)\s*(\w+)\s*\(',old)
    names=[n for n in names if n in ('device_status_encode','device_runtime_encode') or n.startswith(('status_','encode_','device_'))]
    for name in set(names):old=re.sub(r'\b'+name+r'\b','reference_'+name,old)
    for header in ('device_status.h','json_build.h','firmware_variant.h'):old=old.replace('"'+header+'"','"common/'+header+'"')
    text=text.replace('#include "tests/http_core/status_reference.c"','#include "common/device_status.c"\n'+old)
    # Replace just the legacy snapshot function with its production equivalent.
    old_mqtt=(ROOT/'tests/http_core/status_mqtt_reference.c').read_text()
    new_mqtt=(ROOT/'common/mqtt/service.c').read_text()
    text=text.replace(function(old_mqtt,'int service_mqtt_snapshot('),function(new_mqtt,'int service_mqtt_snapshot('))
    product=function((ROOT/'common/service_product.inc').read_text(),'__attribute__((noinline)) struct printbuf *service_product_status_wire(')
    text+='\n#define FEATURE_DEVICE_A1 1\n'+product+'\n'
    # Include actual wire service entry points, sharing the actual validation helper.
    service=(ROOT/'common/service.c').read_text()
    text+='\n#include "common/status_view.h"\nstatic bool ready=true;static mico_mutex_t command_lock;\n'
    text+=function(service,'static bool identifier(')+'\ntypedef struct { const char *id,*action;json_object *params; } service_request;\n'
    text+='\n'.join(function(service,s) for s in ('static bool service_request_decode(','struct printbuf *service_command_wire(','struct printbuf *service_query_wire('))
    # Inject every allocation refusal, including count/fill checked numeric scratch.
    text=text.replace('int json_heap_permit(size_t n){','static unsigned attempts,fault_at;\nint json_heap_permit(size_t n){if(++attempts==fault_at)return 0;')
    return '#define PHICOMM_STRONG_WIRE_FIXTURE 1\n#include "common/ota/mode.h"\n'+text+r'''
#include <stdarg.h>
static int report(const char *format,...){char out[256];va_list args;va_start(args,format);int n=vsnprintf(out,sizeof(out),format,args);va_end(args);return _write(1,out,n);}
#define printf report
static void parity(device_status_t *s,device_status_full_t *f,bool full){
 capacity=libc_baseline+100000;const char *id="read-123";
 json_object *old=reference_device_status_encode(s,full?f:NULL);assert(old);
 if(s->model==DEVICE_A1 && full){
  json_object *old_filter=json_object_object_get(old,"filter"),*new_filter=json_object_new_object();assert(old_filter&&new_filter);
  json_object_object_add(new_filter,"model",json_object_new_string(f->model.a1.filter_stock.weighted?"weighted_estimate":"runtime"));
  json_object_object_add(new_filter,"weighted_usage",json_object_new_int(f->model.a1.filter_stock.usage));
  json_object_object_add(new_filter,"weighted_capacity",json_object_new_int(A1_FILTER_STOCK_CAPACITY));
  json_object_object_add(new_filter,"weighted_remaining_percent",json_object_new_int(a1_filter_stock_remaining(&f->model.a1.filter_stock)));
  json_object_object_add(new_filter,"time_remaining_percent",json_object_new_int(a1_filter_remaining(&f->model.a1.filter)));
  json_object_object_add(new_filter,"air_volume_m3_estimate",json_object_new_double(f->model.a1.filter_stock.air_litres/1000.0));
  json_object_object_foreach(old_filter,k,v){if(v)json_object_get(v);json_object_object_add(new_filter,k,v);}
  json_object_object_add(old,"filter",new_filter);
 }

 /* The legacy oracle predates the additive A1 summary. Keep its existing
  * fields as oracle and compare the new subtree through graph/wire parity. */
 if((s->model==DEVICE_A1 && (s->captured&STATUS_SENSOR)) || s->model==DEVICE_M1){
  json_object *current=device_status_encode(s,full?f:NULL);assert(current);
  json_object *ordered=json_object_new_object();assert(ordered);
  json_object_object_foreach(current,key,value){
   json_object *v=(!strcmp(key,"sensors") || !strcmp(key,"co2_display") || !strcmp(key,"sensor_health"))?value:json_object_object_get(old,key);
   if(v)json_object_get(v);json_object_object_add(ordered,key,v);
  }
  json_object_put(old);old=ordered;json_object_put(current);
 }
 struct printbuf *oracle=json_owned_to_wire(result(id,"ok",old));assert(oracle);
 size_t length=oracle->bpos;char saved[8192];assert(length<sizeof(saved));memcpy(saved,oracle->buf,length+1);printbuf_free(oracle);healthy_stats();
 peak=live;struct printbuf *wire=device_status_wire(s,full?f:NULL,true,id);
 assert(wire&&wire->bpos==(int)length&&!strcmp(saved,wire->buf));
 size_t direct_peak=peak-libc_baseline;printbuf_free(wire);healthy_stats();
 json_object *graph=device_status_encode(s,full?f:NULL);assert(graph);
 wire=json_owned_to_wire(result(id,"ok",graph));assert(wire&&!strcmp(saved,wire->buf));printbuf_free(wire);healthy_stats();
 attempts=0;wire=device_status_wire(s,full?f:NULL,true,id);assert(wire);unsigned points=attempts;printbuf_free(wire);healthy_stats();
 for(unsigned i=1;i<=points;i++) { attempts=0;fault_at=i;wire=device_status_wire(s,full?f:NULL,true,id);fault_at=0;assert(!wire);healthy_stats(); }
 wire=device_status_wire(s,full?f:NULL,true,id);assert(wire&&!strcmp(saved,wire->buf));printbuf_free(wire);healthy_stats();
 printf("direct model=%u full=%u bytes=%u peak=%u allocation-faults=%u parity/recovery PASS\n",s->model,full,(unsigned)length,(unsigned)direct_peak,points);
}
static void states(void){
 device_status_t s={0};device_status_full_t f={0};
 s.captured=STATUS_IDENTITY|STATUS_NETWORK|STATUS_CONTROL|STATUS_SENSOR|STATUS_OTA|STATUS_DIAGNOSTIC;
 s.identity.version="版本/1.0";strcpy(s.identity.name,"UTF8 名称\"\\/\b\n\r\t\x01\x1f");strcpy(s.identity.id,"aabbccddeeff");
 strcpy(s.network.wifi_trial_status,"idle");strcpy(s.ota.error,"\"/\\error");strcpy(s.ota.state,"idle");
 s.now_ms=UINT32_MAX;f.runtime=(device_runtime_t){.now_ms=UINT32_MAX,.memory_valid=true,.free_memory=29000,.reset_valid=true,.reset_reason=-1};
 f.watchdog.reset_name="software";f.watchdog.fault="";strcpy(f.network.ssid,"\"\\/网络");
 for(unsigned i=0;i<6;i++)strcpy(f.model.tc1.socket_names[i],"\"\\/插孔\n\x01");
 for(unsigned model=0;model<3;model++){
  s.model=model;memset(&s.sensor,0,sizeof(s.sensor));
  memset(&f.model,0,sizeof(f.model));
  if(model==DEVICE_A1){a1_filter_defaults(&f.model.a1.filter);a1_filter_stock_defaults(&f.model.a1.filter_stock,false);}
  if(model==DEVICE_TC1)for(unsigned i=0;i<6;i++)strcpy(f.model.tc1.socket_names[i],"插孔\\\"\n");
  parity(&s,&f,false);parity(&s,&f,true);
  s.sensor.m1.sample.received=true;s.sensor.m1.sample.updated_ms=UINT32_MAX-1000;s.sensor.m1.sample.temperature_deci_c=-135;s.sensor.m1.sample.humidity_deci_pct=487;
  s.sensor.m1.co2.ready=s.sensor.m1.co2.received=true;s.sensor.m1.co2.updated_ms=UINT32_MAX-19999;s.sensor.m1.co2.co2_ppm=1234;
  s.sensor.m1.barometer=(barometer_state_t){.ready=true,.received=true,.updated_ms=UINT32_MAX-1,.chip_id=0x50,.address=0x77,.pressure_centi_pa=10132400};
  if(model==DEVICE_A1)s.sensor.a1=(feature_values_t){.value={23,45,123,.1},.co2=1400,.sample_count=2,.co2_sample_count=1,.configured_count=2,.valid=true};
  parity(&s,&f,true);s.now_ms=0;parity(&s,&f,false);s.now_ms=UINT32_MAX;
  if(model==DEVICE_A1){
   /* Actual ARM formatter + both graph/wire paths must preserve good fields
    * and use JSON null for every unavailable metric in a partial sample. */
   for(unsigned field=0;field<5;field++){
    s.sensor.a1=(feature_values_t){.value={23,45,123,.1},.co2=1400,.sample_count=1,.co2_sample_count=1};
    if(field<4)s.sensor.a1.value[field]=NAN;else s.sensor.a1.co2=INFINITY;
    parity(&s,&f,true);parity(&s,&f,false);
    struct printbuf *wire=device_status_wire(&s,NULL,true,"partial");assert(wire);
    const char *null_fields[]={"\"temperature\": null","\"humidity\": null","\"pm25\": null","\"formaldehyde\": null","\"co2\": null"};
    assert(strstr(wire->buf,null_fields[field]));assert(!strstr(wire->buf,": nan")&&!strstr(wire->buf,": inf"));printbuf_free(wire);healthy_stats();
   }
  }
 }
 s.captured=0;assert(!device_status_wire(&s,&f,true,NULL));healthy_stats();
}
static void http_and_memory(void){
 for(unsigned variant=0;variant<3;variant++){
  setup(variant);capacity=libc_baseline+100000;
  for(unsigned post=0;post<2;post++)for(unsigned budget=23000;budget<=29000;budget+=2000){
   capacity=libc_baseline+budget;peak=live;ph_http_product p={0};ph_http_exchange x={0};
   p.failures=service_memory_failures();
   json_object *q=post?json_tokener_parse("{\"action\":\"status.get\",\"params\":{\"view\":\"full\"},\"request_id\":\"status-read\"}"):NULL;
   if(post)ph_http_command(&p,&x,q);else ph_http_query(&p,&x,"status.get");
   json_object_put(q);assert(x.response.status==200&&p.wire);if(post)assert(strstr(p.wire->buf,"status-read"));
   unsigned used=peak-libc_baseline;ph_http_product_release(&p);healthy_stats();
   printf("direct HTTP variant=%u post=%u capacity=%u peak=%u PASS\n",variant,post,budget,used);
  }
 }
 capacity=libc_baseline+100000;setup(0);snapshot_deferred=false;mqtt_bytes=0;
 mqtt_account_t scope;assert(mqtt_account_init(&scope,1,"a1","test","aabbccddeeff"));
 assert(service_mqtt_snapshot(mqtt_sender,&scope)==MQTT_SUCCESS&&mqtt_bytes&&!fixture_scope_token);healthy_stats();
}
int main(void){
 char warm[64];snprintf(warm,sizeof(warm),"%f",1.25);libc_baseline=live;healthy_stats();
 states();http_and_memory();return 0;
}
'''


class DirectStatus(unittest.TestCase):
    def test_actual_arm_wire_parity_faults_and_low_heap(self):
        for model in ('A1','M1','TC1'):
            with self.subTest(model=model):budget.JsonBudget().compile_arm(model,fixture(),budget=True)
