"""Native optional work fairness, isolated writer failures and retry controls."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_http_json_budget import function
ROOT=Path(__file__).resolve().parents[1]
class OptionalJson(unittest.TestCase):
    def test_native_cycle_defer_fairness_and_wrap(self):
        source=(ROOT/'A1/mqtt.c').read_text()
        body=function(source,'static void primary_snapshot(')+'\n'+function(source,'static int discovery(')+'\n'+function(source,'static int session_cycle(')
        fixture=r"""
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 2
#define PH_MQTT_RECONFIGURE 1
typedef struct {unsigned revision,speed;bool fan_on;}a1_config_t;
typedef struct {bool wifi_connected,led_visible;}mqtt_state_t;
typedef struct {unsigned unused;}ha_json;
typedef unsigned mico_thread_t;
typedef struct {unsigned n;}a1_filter_storage_t;
static mico_thread_t mqtt_owner_thread;
static a1_config_t cfg,current;
static mqtt_state_t state;
static bool discovery_deferred,rediscover,sent,old_on,old_visible,deny=true;
static uint8_t discovery_language;
static uint8_t device_language_get(void){return 0;}
static uint32_t discovery_retry_at,now,filter_sent_at;
static unsigned attempts,builds,normal,features,discovery_index,feature_revision;
static unsigned count,state_index,state_pending,state_seen,revision,old_speed;
static a1_filter_storage_t sent_filter;
static char base[1],feature_filters[1];static int publish,publish_bytes,account;
static uint32_t mico_rtos_get_time(void){return now;}
static void *probe_malloc(size_t n){attempts++;return deny?NULL:malloc(n);}
#define malloc probe_malloc
static void ha_json_init(ha_json *w,void *data,size_t n){(void)w;(void)data;(void)n;}
static int discovery_build(const a1_config_t*c,unsigned i,ha_json*s){(void)c;(void)s;assert(i==builds);builds++;return 0;}
static void ota_job_poll(void){}
static int ota_service(const char*b){(void)b;return 0;}
static bool ota_job_active(void){return false;}
static int fan_sensor_cycle(bool on,unsigned speed,bool*handled){(void)on;(void)speed;*handled=false;return 0;}
static void mqtt_state_get(mqtt_state_t*s){s->wifi_connected=true;}
static void a1_config_get(a1_config_t*c){*c=cfg;}
static bool same_connection(const a1_config_t*a,const a1_config_t*b){(void)a;(void)b;return true;}
static uint32_t feature_config_revision(void){return feature_revision;}
static bool feature_mqtt_filters_current(void*f,unsigned n){(void)f;(void)n;return true;}
static int feature_mqtt_service(int p,bool connected){(void)p;assert(connected);features++;return 0;}
static void feature_effective_fan(bool a,unsigned b,bool*c,unsigned*d){*c=a;*d=b;}
static void a1_filter_get(a1_filter_storage_t*f){f->n=0;}
static bool a1_filter_mqtt_due(const a1_filter_storage_t*a,const a1_filter_storage_t*b,uint32_t c,uint32_t d){(void)a;(void)b;(void)c;(void)d;return false;}
/* Discovery fairness fixture: the native producer has no changed input. */
static bool service_mqtt_status_due(bool changed){return changed;}
static bool filter_priority(bool b){return b;}
static int service_mqtt_snapshot(int p,int*a){(void)p;(void)a;return 0;}
static int states_step(const a1_config_t*c,const mqtt_state_t*s,bool b,bool*out){(void)c;(void)s;(void)b;*out=false;normal++;return deny?PH_MQTT_MORE:0;}
/* PRODUCTION */
int main(void){
 now=UINT32_MAX-500;state_pending=127;
 for(unsigned i=0;i<250;i++){assert(session_cycle(NULL)==PH_MQTT_MORE);assert(!discovery_index);now+=20;}
 assert(attempts==5 && !builds && normal==250 && features==250);
 deny=false;
 for(unsigned i=0;i<12;i++){assert(session_cycle(NULL)==0);now+=20;}
 assert(builds==12 && discovery_index==12 && attempts==17);
 /* No duplicated discovery after completion, ordinary work continues. */
 assert(session_cycle(NULL)==0 && builds==12 && normal==251 && features==251);
 /* Even after discovery completed, native full pressure cannot stop features. */
 deny=true;for(unsigned i=0;i<100;i++){assert(session_cycle(NULL)==PH_MQTT_MORE);now+=20;}
 assert(normal==351 && features==351 && discovery_index==12);
}
""".replace('/* PRODUCTION */',body)
        with tempfile.TemporaryDirectory() as d:
            src=Path(d)/'fair.c';src.write_text(fixture);binary=Path(d)/'fair'
            flags=[os.environ.get('CC','cc'),'-std=gnu99','-fsanitize=address,undefined','-fno-pie','-no-pie',str(src),'-o',str(binary)]
            subprocess.run(flags,check=True)
            subprocess.run([str(binary)],check=True,timeout=10)
            mutation=fixture.replace('if(rc!=PH_MQTT_MORE)return rc;\n                rc=MQTT_SUCCESS;', 'return rc;')
            self.assertNotEqual(mutation,fixture)
            src.write_text(mutation)
            subprocess.run(flags,check=True)
            result=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
            self.assertNotEqual(result.returncode,0,'original early return must fail fairness')

    def test_m1_snapshot_pressure_does_not_starve_independent_work(self):
        # Execute only when M1 testing is authorized. Full production cycle,
        # including configuration guards and all normal publication cursors.
        cycle=function((ROOT/'M1/mqtt.c').read_text(),'static int session_cycle(')
        cycle='static int publish_typed_sensors(const void *s,uint32_t now){(void)s;(void)now;return MQTT_SUCCESS;}\n'+cycle
        fixture=r'''
#include "common/typed_wire.h"
#include "common/mqtt_account.h"
#include "common/mqtt/publication.h"
typedef int (*service_mqtt_sender)(const char*,const void*,size_t,bool);
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 2
#define PH_MQTT_RECONFIGURE 1
typedef struct {int temperature_deci_c,humidity_deci_pct;unsigned hcho_ug_m3,pm25_ug_m3,sequence;}m1_sample_t;
typedef struct {bool wifi_connected;unsigned brightness_requested;m1_sample_t sample;barometer_state_t barometer;struct {bool ready,received;unsigned updated_ms,samples,co2_ppm;}scd40;}mqtt_state_t;
typedef struct {unsigned revision;}m1_config_t;
static mqtt_state_t state;static m1_config_t cfg,current;
static bool rediscover,co2_discovered,pressure_discovered;
static uint8_t discovery_language;
static uint8_t device_language_get(void){return 0;}
static bool particles_discovered;
static unsigned m1_lock,particles_at,mqtt_owner_thread,particle_attempts;
static struct {bool mcu_sensor_details;}m1_state;
static void mico_rtos_lock_mutex(unsigned *m){(void)m;}
static void mico_rtos_unlock_mutex(unsigned *m){(void)m;}
static int ha_particles(void){particle_attempts++;return PH_MQTT_MORE;}
static int ha_state(void){return MQTT_SUCCESS;}
static int retry_metadata(void){return MQTT_SUCCESS;}
static unsigned feature_revision,count,discovery_index=MQTT_DISCOVERY_COUNT,initial_index=2,connect_sequence,sequence,co2_sequence,pressure_sequence,snapshot_at,now;
static int old_fresh=-1,old_co2_fresh=-1,old_pressure_fresh=-1,old_brightness=-1,publish;
static mqtt_account_t account;
static char base[1],feature_filters[1];
static unsigned snapshots,sensors,features;static int snapshot_result=PH_MQTT_MORE;
static bool connection_ok=true,filters_ok=true;
static uint32_t mico_rtos_get_time(void){return now;}
static void ota_job_poll(void){}
static bool ota_job_active(void){return false;}
static int ota_service(const char*x){(void)x;return 0;}
static bool snapshot_deferred,capabilities_pending;
static ph_mqtt_publication status_publication,ha_publication;
static uint32_t snapshot_retry_at;
#define PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED 9
static int ph_mqtt_resource_ready(void){return MQTT_SUCCESS;}
static int publish_bytes(const char *topic,const void *data,size_t length,bool retained){
 assert(strstr(topic,"/state")&&retained);ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,data,length,&v)&&v.kind==PHV_MAP);
 assert(memchr(data,0,length));return snapshot_result;
}
static ph_buf *service_query_mp(const char *action){
 assert(!strcmp(action,"status.get"));snapshots++;
 if(snapshot_result==PH_MQTT_MORE)return NULL;
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);
 phw_u32(&w,"sequence",0);phw_text(&w,"model","m1");phw_end(&w);return phw_take(&w);
}
/* SNAPSHOT */
static void mqtt_state_get(mqtt_state_t*s){s->wifi_connected=true;s->sample.sequence++;}
static void m1_config_get(m1_config_t*c){*c=cfg;}
static bool same_connection(const m1_config_t*a,const m1_config_t*b){(void)a;(void)b;return connection_ok;}
static uint32_t feature_config_revision(void){return cfg.revision;}
static bool feature_mqtt_filters_current(void*f,unsigned n){(void)f;(void)n;return filters_ok;}
static int discovery(const m1_config_t*c,unsigned i){(void)c;(void)i;assert(0);return -1;}
static bool m1_sample_fresh(const m1_sample_t*s,uint32_t t){(void)s;(void)t;return true;}
static int publish_leaf(const char*leaf,const char*value,bool retain){(void)value;(void)retain;if(!__builtin_strcmp(leaf,"sensor"))sensors++;return 0;}
static int feature_mqtt_service(service_mqtt_sender p,bool connected){(void)p;assert(connected);features++;return 0;}
/* PRODUCTION */
int main(void){
 assert(mqtt_account_init(&account,1,"m1","test","aabbccddeeff"));
 now=UINT32_MAX-2500;snapshot_at=now-5000u;
 for(unsigned i=0;i<600;i++){assert(session_cycle(NULL)==0);now+=20;}
 assert(snapshots==12 && sensors==600 && features==600);
 assert(particle_attempts==4); /* Bounded optional retries across clock rollover. */
 /* Native retry is bounded to one second, independent of HA completion. */
 snapshot_result=0;now=status_publication.attempted_at+1000u;
 assert(session_cycle(NULL)==0 && snapshots==13 && sensors==601 && features==601);
 /* Actual SDK error remains fatal and cannot silently become a deferral. */
 snapshot_result=-17;now+=5000u;
 assert(session_cycle(NULL)==-17 && sensors==601 && features==601);
 snapshot_result=PH_MQTT_MORE;connection_ok=false;now++;
 assert(session_cycle(NULL)==PH_MQTT_RECONFIGURE);
 connection_ok=true;cfg.revision++;filters_ok=false;
 /* M1 no longer subscribes to peer filters; binding revisions keep the session. */
 assert(session_cycle(NULL)==MQTT_SUCCESS);
}
'''.replace('/* PRODUCTION */',cycle)
        service=(ROOT/'common/mqtt/service.c').read_text()
        fixture=fixture.replace('/* SNAPSHOT */','\n'.join(function(service,s) for s in ('static int snapshot_wait(', 'static int publish_query(', 'static int try_capabilities(', 'int service_mqtt_snapshot(', 'bool service_mqtt_status_due(')))
        with tempfile.TemporaryDirectory() as d:
            source=(ROOT/'M1/mqtt.c').read_text()
            fixture='#include \"'+str(ROOT/'M1/barometer.h')+'\"\n'+source[source.index('enum { MQTT_BRIGHTNESS_INDEX'):source.index(';',source.index('enum { MQTT_BRIGHTNESS_INDEX'))+1]+'\n'+fixture
            src=Path(d)/'m1-fair.c';binary=Path(d)/'m1-fair';src.write_text(fixture)
            flags=[os.environ.get('CC','cc'),'-std=gnu99','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT),str(src),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(binary)]
            subprocess.run(flags,check=True)
            subprocess.run([str(binary)],check=True,timeout=10)
            begin=fixture.index('            if(!ota_job_active() &&')
            end=fixture.index('            rc=ota_service(base);',begin)
            mutation=fixture[:begin]+'''            if(!ota_job_active() && (uint32_t)(mico_rtos_get_time()-snapshot_at)>=5000){rc=service_mqtt_snapshot(publish_bytes,&account);if(rc!=MQTT_SUCCESS)return rc;snapshot_at=mico_rtos_get_time();}
'''+fixture[end:]
            src.write_text(mutation);subprocess.run(flags,check=True)
            result=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
            self.assertNotEqual(result.returncode,0,'original snapshot early return must fail fairness')

    def test_scoped_resource_and_structural_failures(self):
        # Native writers own their failure flag: no global JSON allocation
        # observer/token exists. A foreign writer failure cannot taint a reply.
        fixture=r'''
#include "common/typed_wire.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static __thread unsigned calls,fail_at;static unsigned live;
void *ph_wire_malloc(size_t n){if(++calls==fail_at)return NULL;void *p=malloc(n);if(p)__atomic_add_fetch(&live,1,__ATOMIC_RELAXED);return p;}
void ph_wire_free(void *p){if(p){assert(__atomic_load_n(&live,__ATOMIC_RELAXED));__atomic_sub_fetch(&live,1,__ATOMIC_RELAXED);free(p);}}
static void *foreign(void *unused){
 (void)unused;ph_writer w;calls=0;fail_at=1;assert(!phw_init(&w,1));assert(!phw_take(&w));return NULL;
}
int main(void){
 for(unsigned boundary=1;boundary<=5;boundary++){
  ph_writer w;calls=0;fail_at=boundary;
  bool opened=phw_init_capacity(&w,1,16);
  if(opened){phw_begin(&w,NULL,false);phw_text(&w,"value","abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");phw_end(&w);}
  ph_buf *b=phw_take(&w);
  if(b){ph_value root;assert(phv_parse(1,b->buf,b->len,&root));ph_buf_free(b);}
  assert(!live);
 }
 fail_at=0;calls=0;ph_writer owner;assert(phw_init(&owner,1));phw_begin(&owner,NULL,false);
 pthread_t thread;assert(!pthread_create(&thread,NULL,foreign,NULL));assert(!pthread_join(thread,NULL));
 phw_bool(&owner,"complete",true);phw_end(&owner);ph_buf *reply=phw_take(&owner);assert(reply);ph_buf_free(reply);assert(!live);
 char storage[32];ph_buf fixed;ph_writer w;assert(phw_init_fixed(&w,1,&fixed,storage,sizeof(storage)));
 phw_begin(&w,NULL,false);phw_text(&w,"oversized","abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");assert(w.failed);
 phw_bool(&w,"later",true);phw_end(&w);assert(!phw_finish_fixed(&w)); /* sticky, never partial publication */
 assert(phw_init_fixed(&w,1,&fixed,storage,sizeof(storage)));
 for(unsigned i=0;i<13;i++)phw_begin(&w,NULL,true);assert(w.failed&&!phw_finish_fixed(&w));
 ph_value root;const unsigned char duplicate[]={0x82,0xa1,'x',1,0xa1,'x',2};assert(!phv_parse(1,duplicate,sizeof(duplicate),&root));
 assert(!live);return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);src=p/'native.c';src.write_text(fixture);exe=p/'native'
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT),str(src),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
