"""Complete binary status vs the frozen 6950338 JSON visitor, by decoded schema."""
import os
import io
import json as json_module
import tarfile
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from tests.sdk_json import copy_patched_json

ROOT=Path(__file__).resolve().parents[1]
SOURCE=Path(os.environ.get('NATIVE_MP_ROOT',ROOT))

HARNESS=r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/device_status.h"
#include "json.h"
#include "json_heap.h"
#include "printbuf.h"
struct printbuf *reference_device_status_wire(const device_status_t *,const device_status_full_t *,bool,const char *);
static long budget=-1,blocks=0;
static size_t live_bytes,peak_bytes,wire_ceiling=SIZE_MAX;
typedef union {size_t bytes;long double aligned;void *pointer;} allocation_header;
void *ph_wire_malloc(size_t n){
 if(!budget||live_bytes>wire_ceiling||n>wire_ceiling-live_bytes)return NULL;
 if(budget>0)--budget;allocation_header *p=malloc(sizeof(*p)+n);
 if(p){p->bytes=n;blocks++;live_bytes+=n;if(live_bytes>peak_bytes)peak_bytes=live_bytes;}
 return p?p+1:NULL;
}
void ph_wire_free(void *p){if(p){allocation_header *h=(allocation_header *)p-1;assert(blocks&&live_bytes>=h->bytes);blocks--;live_bytes-=h->bytes;free(h);}}
static void same(const ph_value *value,json_object *expected){
 if(!expected){assert(value->kind==PHV_NULL);return;}
 enum json_type type=json_object_get_type(expected);
 switch(value->kind){
 case PHV_MAP:{assert(type==json_type_object);size_t count=0;
  json_object_object_foreach(expected,key,old){ph_value got;if(!phv_get(value,key,&got)){fprintf(stderr,"missing field %s\n",key);abort();}same(&got,old);count++;}
  assert(count==value->count);break;}
 case PHV_ARRAY:{assert(type==json_type_array);assert((size_t)json_object_array_length(expected)==value->count);
  for(unsigned i=0;i<value->count;i++){ph_value got;assert(phv_at(value,i,&got));same(&got,json_object_array_get_idx(expected,i));}break;}
 case PHV_TEXT:assert(type==json_type_string&&phv_equal_text(value,json_object_get_string(expected)));break;
 case PHV_NULL:assert(!expected||type==json_type_null);break;
 case PHV_TRUE:case PHV_FALSE:assert(type==json_type_boolean&&json_object_get_boolean(expected)==(value->kind==PHV_TRUE));break;
 default:{assert(type==json_type_int||type==json_type_double);
  if(value->kind!=PHV_REAL&&type==json_type_int){int64_t exact;assert(phv_int(value,&exact)&&exact==json_object_get_int64(expected));break;}
  double actual;assert(phv_number(value,&actual));double old=json_object_get_double(expected);
  /* The frozen JSON wire used six-significant-digit %g; MessagePack retains
   * binary64. Compare within that explicit old rounding bound, not exact text. */
  assert(fabs(actual-old)<=fmax(fabs(actual),1.0)*0.0000051);break;}
 }
}
static void check(device_status_t *s,device_status_full_t *f){
 f->runtime.now_ms=s->now_ms;
 struct printbuf *old=reference_device_status_wire(s,f,false,NULL);assert(old);
 json_object *schema=json_tokener_parse(old->buf);assert(schema);printbuf_free(old);
 /* Explicit RC4 policy delta: frozen rc3 reported one waiting request. */
 json_object *old_queue=json_object_object_get(schema,"http_accept_queue_capacity");
 if(old_queue){assert(json_object_get_int(old_queue)==1);json_object_object_add(schema,"http_accept_queue_capacity",json_object_new_int(PH_HTTP_WAIT_SLOTS));}
 /* Reunite independently emitted diagnostics with business schema to prove
  * no old field or numeric precision was silently lost during the split. */
 ph_writer diag;assert(phw_init(&diag,1));device_diagnostics_mp(&diag,s,f);
 ph_buf *diagnostics=phw_take(&diag);ph_value dr,fields,item,id,value,version;
 assert(diagnostics&&phv_parse(1,diagnostics->buf,diagnostics->len,&dr));
 assert(phv_get(&dr,"schema",&version)&&version.count==1);
 assert(phv_get(&dr,"fields",&fields)&&fields.kind==PHV_ARRAY);
 bool seen_ids[1024]={0};
 for(unsigned n=0;n<fields.count;n++){
  assert(phv_at(&fields,n,&item)&&item.kind==PHV_ARRAY&&item.count==2);
  assert(phv_at(&item,0,&id)&&id.kind==PHV_UINT&&id.count<1024&&!seen_ids[id.count]);seen_ids[id.count]=true;
  assert(phv_at(&item,1,&value));
  const char *path=diagnostic_paths[id.count];assert(path);
  char key[128];strcpy(key,path);char *leaf=strchr(key,'.');json_object *parent=schema;
  if(leaf){*leaf++=0;parent=json_object_object_get(schema,key);}else leaf=key;
  json_object *old=parent?json_object_object_get(parent,leaf):NULL;
  bool present=false;if(parent){json_object_object_foreach(parent,k,v){(void)v;if(!strcmp(k,leaf))present=true;}}
  if(present){
   if(!strcmp(path,"http_tcp_limit_requested")){assert(value.count==PH_HTTP_WAIT_SLOTS+1);}
   else same(&value,old);
   if(strcmp(path,"uptime_seconds"))json_object_object_del(parent,leaf);
  }
 }
 /* Unsupported legacy nil placeholders are omitted, never fabricated. */
 json_object_object_del(schema,"http_accept_queue_last_rc");
 if(f->http.limit_get_rc)json_object_object_del(schema,"http_tcp_limit_readback");
 if(f->http.connections_get_rc)json_object_object_del(schema,"http_tcp_connections");
 if(s->model==DEVICE_M1&&!m1_barometer_model(s->sensor.m1.barometer.chip_id)){
  json_object *barometer=json_object_object_get(schema,"barometer");
  json_object_object_del(barometer,"chip_id");json_object_object_del(barometer,"address");
 }
 ph_buf_free(diagnostics);
 ph_writer writer;assert(phw_init(&writer,1));device_status_mp(&writer,s,f);ph_buf *wire=phw_take(&writer);assert(wire&&wire->cap==wire->len+1);
 ph_value root;assert(phv_parse(1,wire->buf,wire->len,&root));same(&root,schema);
 ph_writer measured;ph_buf size;assert(phw_init_measure(&measured,1,&size));budget=0;device_status_mp(&measured,s,f);budget=-1;assert(phw_finish_fixed(&measured)==wire->len);
 char storage[8192];ph_writer fixed;ph_buf borrowed;assert(wire->len<sizeof(storage));assert(phw_init_fixed(&fixed,1,&borrowed,storage,sizeof(storage)));budget=0;device_status_mp(&fixed,s,f);budget=-1;assert(phw_finish_fixed(&fixed)==wire->len&&!memcmp(storage,wire->buf,wire->len));
 /* Frozen encoder must be deterministic, pure, and safe for every refusal. */
 ph_writer again;assert(phw_init(&again,1));device_status_mp(&again,s,f);ph_buf *copy=phw_take(&again);assert(copy&&wire->len==copy->len&&!memcmp(wire->buf,copy->buf,wire->len));ph_buf_free(copy);
 ph_writer wrapped;assert(phw_init(&wrapped,1));phw_begin(&wrapped,NULL,false);phw_root_key(&wrapped,"data");device_status_mp(&wrapped,s,f);phw_end(&wrapped);copy=phw_take(&wrapped);assert(copy);ph_buf_free(copy);
 /* Capabilities can already occupy heap when the first status is requested.
  * Keep that owned buffer resident and allow only this frozen status's exact
  * final bytes plus the initial 128-byte scratch, not a geometric-growth pair. */
 ph_buf *resident_caps=ph_buf_new(1800);assert(resident_caps);size_t held_bytes=live_bytes;
 wire_ceiling=held_bytes+sizeof(ph_buf)+wire->len+1+PHW_INITIAL_CAPACITY;
 peak_bytes=live_bytes;ph_writer cold;assert(phw_init(&cold,1));device_status_mp(&cold,s,f);
 copy=phw_take(&cold);assert(copy&&copy->len==wire->len&&!memcmp(copy->buf,wire->buf,wire->len));
 assert(peak_bytes<=wire_ceiling);ph_buf_free(copy);assert(live_bytes==held_bytes);
 wire_ceiling=SIZE_MAX;ph_buf_free(resident_caps);
 size_t saved=blocks;for(int fail=0;fail<6;fail++){
  budget=fail;ph_writer cold_failure;if(phw_init(&cold_failure,1)){device_status_mp(&cold_failure,s,f);copy=phw_take(&cold_failure);if(copy)ph_buf_free(copy);}else phw_discard(&cold_failure);
  budget=-1;assert((size_t)blocks==saved);
 }
 for(int fail=0;fail<20;fail++){budget=fail;ph_writer w;if(phw_init_capacity(&w,1,1)){device_status_mp(&w,s,f);copy=phw_take(&w);if(copy)ph_buf_free(copy);}else phw_discard(&w);budget=-1;assert((size_t)blocks==saved);}
 ph_buf_free(wire);json_object_put(schema);assert(!blocks);json_heap_stats_t heap;json_heap_snapshot(&heap);assert(!heap.live_blocks);
}
int main(void){
 device_status_t s={0};device_status_full_t f={0};
 s.captured=STATUS_IDENTITY|STATUS_NETWORK|STATUS_CONTROL|STATUS_SENSOR|STATUS_OTA|STATUS_DIAGNOSTIC;
 s.identity.version="rc4/版本";strcpy(s.identity.name,"name\"\\\n\x01");strcpy(s.identity.id,"aabbccddeeff");s.now_ms=UINT32_MAX;
 strcpy(s.network.wifi_trial_status,"idle");s.network.wifi_connected=true;s.network.mqtt_connected=true;s.network.mqtt_status=5;
#ifdef PHICOMM_BEMFA
 s.network.mqtt_enabled=true;s.network.bemfa_enabled=true;s.network.bemfa_status=4;s.network.bemfa_errors=42;s.network.bemfa_last_error=-3;
#endif
 strcpy(s.ota.state,"idle");strcpy(s.ota.error,"\"/error\\");s.ota.progress=71;
 strcpy(f.network.ssid,"网络\"\\\n");strcpy(f.network.ip,"10.1.2.3");strcpy(f.network.mqtt_host,"host");strcpy(f.network.mqtt_user,"user");f.network.mqtt_port=1883;
 f.network.web_password_set=true;f.network.mqtt_errors=77;
 f.runtime=(device_runtime_t){.now_ms=UINT32_MAX,.memory_valid=true,.free_memory=30000,.allocated_memory=10000,.minimum_free=23456,.reset_valid=true,.reset_reason=-1};
 f.watchdog.fault="";f.watchdog.reset_name="software";f.watchdog.enabled=true;f.watchdog.ready=true;
 strcpy(f.ota.release_counter,"18446744073709551615");strcpy(f.ota.version,"rc4");f.ota.signature_verified=true;
 for(unsigned model=0;model<3;model++){
  s.model=model;memset(&s.control,0,sizeof(s.control));memset(&s.sensor,0,sizeof(s.sensor));memset(&f.model,0,sizeof(f.model));
  if(model==DEVICE_A1){a1_filter_defaults(&f.model.a1.filter);a1_filter_stock_defaults(&f.model.a1.filter_stock,true);f.model.a1.filter_stock.usage=123456;f.model.a1.filter_stock.air_litres=234567;f.model.a1.panel_raw[0]=0xab;f.model.a1.panel_raw[1]=0;f.model.a1.panel_raw[2]=0xff;
   s.control.a1.speed=80;s.control.a1.effective_on=true;s.control.a1.effective_speed=79;s.control.a1.red=255;
#ifdef A1_FRESH_AIR
   s.control.a1.fresh=(fresh_snapshot_t){.requested_on=true,.requested_speed=81,.applied=64,.target=64,.duty=63.25,.rpm=1234,.pulses=82};
#endif
  }
  if(model==DEVICE_TC1){for(unsigned i=0;i<6;i++){strcpy(f.model.tc1.socket_names[i],"插孔\"\\\n");s.control.tc1.states[i]=(int)i%2;}f.model.tc1.modern=true;f.model.tc1.utc_time=1722000000;f.model.tc1.time_synced=true;f.model.tc1.report_seconds=30;f.model.tc1.power_coefficient=105;}
  check(&s,&f);
  if(model==DEVICE_A1){s.sensor.a1=(feature_values_t){.value={23.45678f,45,123,.12345678f},.co2=1400,.sample_count=1,.co2_sample_count=1,.configured_count=3,.valid=true};}
  if(model==DEVICE_M1){s.sensor.m1.sample=(m1_sample_t){.received=true,.updated_ms=UINT32_MAX-100,.temperature_deci_c=-135,.humidity_deci_pct=487,.pm25_ug_m3=31,.hcho_ug_m3=127,.pm25_status=1,.climate_status=1};s.sensor.m1.co2.ready=s.sensor.m1.co2.received=true;s.sensor.m1.co2.updated_ms=UINT32_MAX-10;s.sensor.m1.co2.co2_ppm=1200;
   s.sensor.m1.barometer=(barometer_state_t){.ready=true,.received=true,.chip_id=0x50,.pressure_centi_pa=10132456,.updated_ms=UINT32_MAX-10};s.sensor.m1.co2_display=(device_m1_co2_t){.supported=true,.present=true,.valid=true,.ppm=1401,.revision=3};for(unsigned i=0;i<20;i++)f.model.m1.last_frame[i]=(unsigned char)(i*13);f.model.m1.ntp_synced=true;f.model.m1.ntp_last_success=1722000000;}
  check(&s,&f);
  if(model==DEVICE_A1){for(unsigned missing=0;missing<5;missing++){if(missing<4)s.sensor.a1.value[missing]=NAN;else s.sensor.a1.co2=INFINITY;check(&s,&f);}}
  s.now_ms=20000;check(&s,&f);s.now_ms=UINT32_MAX;
 }
 ph_writer w;assert(phw_init(&w,1));device_status_mp(&w,&s,NULL);assert(!phw_take(&w));
 s.captured=0;assert(phw_init(&w,1));device_status_mp(&w,&s,&f);assert(!phw_take(&w));
 assert(phw_init(&w,1));phw_begin(&w,NULL,false);device_runtime_mp(&w,&f.runtime);phw_end(&w);ph_buf *runtime=phw_take(&w);assert(runtime);ph_buf_free(runtime);assert(!blocks);return 0;
}
'''

DIAGNOSTIC_PATHS='static const char *const diagnostic_paths[1024]={'+','.join('[%d]=%s'%(f['id'],json_module.dumps(f['path'])) for f in json_module.loads((ROOT/'common/diagnostics-schema.json').read_text())['fields'])+'};'

class StatusMessagePack(unittest.TestCase):
 def test_frozen_full_schema_variants_faults_and_precision(self):
  sdk=Path(os.environ.get('M1_SDK_DIR',ROOT/'build/m1-sdk/mico-os'))
  source=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',sdk/'libraries/utilities/json_c'))
  self.assertTrue(source.is_dir(),'prepare pinned M1 SDK or set M1_SDK_DIR / PHICOMM_TEST_JSON_DIR')
  for fresh,resources,bemfa in ((0,1,0),(0,1,1),(0,0,1),(1,1,1),(1,0,1)):
   with self.subTest(fresh=fresh,resources=resources,bemfa=bemfa),tempfile.TemporaryDirectory(prefix='status-mp-') as tmp:
    path=Path(tmp)
    selected=source
    # Read the pinned Git object, not a concurrently modified product SDK.
    # An explicit already prepared JSON overlay remains supported for CI.
    if 'PHICOMM_TEST_JSON_DIR' not in os.environ and (sdk/'.git').exists():
     pinned=json_module.loads((ROOT/'common/mico-sdk.json').read_text())['commit']
     revision=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
     self.assertEqual(revision,pinned)
     archive=subprocess.check_output(['git','-C',str(sdk),'archive',pinned,'libraries/utilities/json_c'])
     with tarfile.open(fileobj=io.BytesIO(archive)) as contents:contents.extractall(path/'pristine',filter='data')
     selected=path/'pristine/libraries/utilities/json_c'
    json=copy_patched_json(selected,path/'sdk')
    for p in json.glob('*.c'):p.write_text(p.read_text().replace('%ld','%d'))
    fixture=path/'case.c';fixture.write_text(HARNESS.replace('static long budget=',DIAGNOSTIC_PATHS+'\nstatic long budget='))
    cc=shlex.split(os.environ.get('CC','cc'))
    flags=['-std=gnu99','-g','-Wall','-Wextra','-Wno-misleading-indentation','-Wno-unused-function','-Wno-unused-parameter','-Wno-sign-compare','-Wno-implicit-fallthrough','-Wno-unused-value','-Wno-unused-but-set-variable','-Wno-nonnull-compare','-I'+str(ROOT),'-I'+str(ROOT/'common'),'-I'+str(ROOT/'tests/stubs'),'-I'+str(json),'-DPHICOMM_APPLICATION_WATCHDOG=1','-DPHICOMM_FULL_RESOURCES='+str(resources)]
    if fresh:flags+=['-DA1_FRESH_AIR=1']
    if bemfa:flags+=['-DPHICOMM_BEMFA=1']
    if os.environ.get('SANITIZE','0')=='1':flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
    sources=[fixture,SOURCE/'common/device_status.c',SOURCE/'common/typed_wire.c',SOURCE/'common/wire_buffer.c',ROOT/'tests/status_msgpack/reference_status.c',*json.glob('*.c')]
    built=subprocess.run(cc+flags+[str(p) for p in sources]+['-lm','-o',str(path/'case')],capture_output=True,text=True,timeout=60)
    self.assertEqual(built.returncode,0,built.stdout+built.stderr)
    ran=subprocess.run([str(path/'case')],capture_output=True,text=True,timeout=60)
    self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
