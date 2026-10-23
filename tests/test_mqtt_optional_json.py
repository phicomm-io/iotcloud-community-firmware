"""Production optional graph attribution, allocator and encoder negative controls."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.sdk_json import copy_patched_json
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
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 2
#define PH_MQTT_RECONFIGURE 1
typedef struct {unsigned revision,speed;bool fan_on;}a1_config_t;
typedef struct {bool wifi_connected,led_visible;}mqtt_state_t;
typedef struct {unsigned token,claim;}service_mqtt_json_scope;
typedef unsigned mico_thread_t;
typedef struct {unsigned n;}a1_filter_storage_t;
static mico_thread_t mqtt_owner_thread;
static a1_config_t cfg,current;
static mqtt_state_t state;
static bool discovery_deferred,rediscover,sent,old_on,old_visible,deny=true;
static uint32_t discovery_retry_at,now,filter_sent_at;
static unsigned attempts,builds,normal,features,discovery_index,feature_revision;
static unsigned count,state_index,state_pending,state_seen,revision,old_speed;
static a1_filter_storage_t sent_filter;
static char base[1],feature_filters[1];static int publish,account;
static uint32_t mico_rtos_get_time(void){return now;}
static bool service_mqtt_json_begin(service_mqtt_json_scope*s,mico_thread_t*t){(void)s;(void)t;attempts++;return !deny;}
static void service_mqtt_json_end(service_mqtt_json_scope*s){(void)s;}
static int discovery_build(const a1_config_t*c,unsigned i,service_mqtt_json_scope*s){(void)c;(void)s;assert(i==builds);builds++;return 0;}
static void ota_job_poll(void){}
static int ota_service(const char*b){(void)b;return 0;}
static bool ota_job_active(void){return false;}
static void mqtt_state_get(mqtt_state_t*s){s->wifi_connected=true;}
static void a1_config_get(a1_config_t*c){*c=cfg;}
static bool same_connection(const a1_config_t*a,const a1_config_t*b){(void)a;(void)b;return true;}
static uint32_t feature_config_revision(void){return feature_revision;}
static bool feature_mqtt_filters_current(void*f,unsigned n){(void)f;(void)n;return true;}
static int feature_mqtt_service(int p,bool connected){(void)p;assert(connected);features++;return 0;}
static void feature_effective_fan(bool a,unsigned b,bool*c,unsigned*d){*c=a;*d=b;}
static void a1_filter_get(a1_filter_storage_t*f){f->n=0;}
static bool a1_filter_mqtt_due(const a1_filter_storage_t*a,const a1_filter_storage_t*b,uint32_t c,uint32_t d){(void)a;(void)b;(void)c;(void)d;return false;}
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
        fixture=r'''
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
static unsigned feature_revision,count,discovery_index=MQTT_DISCOVERY_COUNT,initial_index=2,connect_sequence,sequence,co2_sequence,pressure_sequence,snapshot_at,now;
static int old_fresh=-1,old_co2_fresh=-1,old_pressure_fresh=-1,old_brightness=-1,publish,account;
static char base[1],feature_filters[1];
static unsigned snapshots,sensors,features;static int snapshot_result=PH_MQTT_MORE;
static bool connection_ok=true,filters_ok=true;
static uint32_t mico_rtos_get_time(void){return now;}
static void ota_job_poll(void){}
static bool ota_job_active(void){return false;}
static int ota_service(const char*x){(void)x;return 0;}
static int service_mqtt_snapshot(int p,int*a){(void)p;(void)a;snapshots++;return snapshot_result;}
static void mqtt_state_get(mqtt_state_t*s){s->wifi_connected=true;s->sample.sequence++;}
static void m1_config_get(m1_config_t*c){*c=cfg;}
static bool same_connection(const m1_config_t*a,const m1_config_t*b){(void)a;(void)b;return connection_ok;}
static uint32_t feature_config_revision(void){return cfg.revision;}
static bool feature_mqtt_filters_current(void*f,unsigned n){(void)f;(void)n;return filters_ok;}
static int discovery(const m1_config_t*c,unsigned i){(void)c;(void)i;assert(0);return -1;}
static bool m1_sample_fresh(const m1_sample_t*s,uint32_t t){(void)s;(void)t;return true;}
static int publish_leaf(const char*leaf,const char*value,bool retain){(void)value;(void)retain;if(!__builtin_strcmp(leaf,"sensor"))sensors++;return 0;}
static int feature_mqtt_service(int p,bool connected){(void)p;assert(connected);features++;return 0;}
/* PRODUCTION */
int main(void){
 now=UINT32_MAX-2500;snapshot_at=now-5000u;
 for(unsigned i=0;i<600;i++){assert(session_cycle(NULL)==0);now+=20;}
 assert(snapshots==3 && sensors==600 && features==600);
 /* Recovery at the next original period; no repeated full state attempts. */
 snapshot_result=0;now=snapshot_at+5000u;
 assert(session_cycle(NULL)==0 && snapshots==4 && sensors==601 && features==601);
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
        with tempfile.TemporaryDirectory() as d:
            source=(ROOT/'M1/mqtt.c').read_text()
            fixture='#include \"'+str(ROOT/'M1/barometer.h')+'\"\n'+source[source.index('enum { MQTT_BRIGHTNESS_INDEX'):source.index(';',source.index('enum { MQTT_BRIGHTNESS_INDEX'))+1]+'\n'+fixture
            src=Path(d)/'m1-fair.c';binary=Path(d)/'m1-fair';src.write_text(fixture)
            flags=[os.environ.get('CC','cc'),'-std=gnu99','-fsanitize=address,undefined','-fno-pie','-no-pie',str(src),'-o',str(binary)]
            subprocess.run(flags,check=True)
            subprocess.run([str(binary)],check=True,timeout=10)
            begin=fixture.index('            if(!ota_job_active() &&')
            end=fixture.index('            rc=ota_service(base);',begin)
            mutation=fixture[:begin]+'''            if(!ota_job_active() && (uint32_t)(mico_rtos_get_time()-snapshot_at)>=5000){rc=service_mqtt_snapshot(publish,&account);if(rc!=MQTT_SUCCESS)return rc;snapshot_at=mico_rtos_get_time();}
'''+fixture[end:]
            src.write_text(mutation);subprocess.run(flags,check=True)
            result=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
            self.assertNotEqual(result.returncode,0,'original snapshot early return must fail fairness')

    def test_scoped_resource_and_structural_failures(self):
        source=(ROOT/'common/mqtt/service.c').read_text()
        names=('bool service_mqtt_json_begin(',
               'static unsigned service_mqtt_json_finish(', 'void service_mqtt_json_end(',
               'int service_mqtt_json_error(', 'int service_mqtt_optional_send(')
        service=(ROOT/'common/service.c').read_text()
        functions='static struct {unsigned token,failed;void *owner;} json_scope;\n'+'\n'.join(function(service,n) for n in ('void json_heap_failure(', 'bool service_json_scope_begin(', 'unsigned service_json_scope_failed(', 'unsigned service_json_scope_end('))+'\n'+'\n'.join(function(source,n) for n in names)
        fixture=r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common/json_owned.h"
#include "common/json_build.h"
#include "json_heap.h"
#include "json_object_private.h"
#include "printbuf.h"
#define PH_MQTT_MORE 2
#define MQTT_FAILURE -1
#define PH_MQTT_ERROR_APP_DISCOVERY_ENCODING 10
#define PH_MQTT_ERROR_APP_DISCOVERY_GRAPH 8
typedef pthread_t mico_thread_t;
typedef struct {unsigned token,claim;}service_mqtt_json_scope;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER,service_lock=PTHREAD_MUTEX_INITIALIZER;
static bool ready=true;static unsigned json_owner;
static void mico_rtos_lock_mutex(pthread_mutex_t *m){assert(!pthread_mutex_lock(m));}
static void mico_rtos_unlock_mutex(pthread_mutex_t *m){assert(!pthread_mutex_unlock(m));}
static bool busy,deny_claim,token_live,claim_live,deny_alloc,oom;
static unsigned tags,sends;
void json_heap_lock(void){assert(!pthread_mutex_lock(&lock));}
void json_heap_unlock(void){assert(!pthread_mutex_unlock(&lock));}
int json_heap_permit(size_t n){(void)n;return !deny_alloc;}
void *__real_malloc(size_t n);
void *__real_calloc(size_t n,size_t m);
void *__wrap_calloc(size_t n,size_t m){if(oom){oom=false;return NULL;}return __real_calloc(n,m);}
void *__wrap_malloc(size_t n){if(oom){oom=false;return NULL;}return __real_malloc(n);}
static bool mico_rtos_is_current_thread(mico_thread_t *t){return pthread_equal(*t,pthread_self());}
static unsigned service_json_claim(void){if(busy)return 0;assert(!token_live);token_live=true;json_owner=1;return 1;}
static unsigned service_memory_claim(size_t n){(void)n;if(deny_claim)return 0;assert(!claim_live);claim_live=true;return 1;}
static void service_json_release(unsigned t){assert(t&&token_live);token_live=false;json_owner=0;}
static void service_memory_release(unsigned t){assert(t&&claim_live);claim_live=false;}
static int ph_mqtt_app_error(unsigned stage,int rc){assert(stage==8||stage==10);tags++;return rc;}
/* PRODUCTION */
static mico_thread_t owner;
static service_mqtt_json_scope scope;
static void clean(void){json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks&&!token_live&&!claim_live&&!json_scope.owner);}
static int send(const char *t,const char *w,bool r){assert(t&&w&&r&&!token_live&&!claim_live&&!json_scope.owner);sends++;return -17;}
static int bad_encoder(json_object *o,struct printbuf *p){(void)o;(void)p;return -1;}
static void *foreign(void *arg){(void)arg;deny_alloc=true;assert(!json_object_new_object());deny_alloc=false;return NULL;}
int main(void){
 owner=pthread_self();busy=true;assert(!service_mqtt_json_begin(&scope,&owner));busy=false;clean();
 deny_claim=true;assert(!service_mqtt_json_begin(&scope,&owner));deny_claim=false;clean();
 for(unsigned kind=0;kind<2;kind++){
  assert(service_mqtt_json_begin(&scope,&owner));deny_alloc=kind==0;oom=kind==1;
  assert(!json_object_new_object());deny_alloc=false;
  assert(service_mqtt_json_error(&scope,8)==PH_MQTT_MORE&&!tags);clean();
 }
 assert(service_mqtt_json_begin(&scope,&owner));
 pthread_t other;assert(!pthread_create(&other,NULL,foreign,NULL));assert(!pthread_join(other,NULL));
 json_object *o=json_object_new_object();assert(o);json_build_object_add(o,"required",NULL);
 assert(service_mqtt_optional_send(send,"topic",o,true,&scope)==-1&&tags==1&&!sends);clean();
 /* A definitive excessive-depth error dominates even an earlier own denial. */
 o=json_object_new_object();json_object *cursor=o;
 for(unsigned i=0;i<33;i++){json_object *child=json_object_new_object();json_build_object_add(cursor,"child",child);cursor=child;}
 assert(service_mqtt_json_begin(&scope,&owner));deny_alloc=true;assert(!json_object_new_object());deny_alloc=false;
 assert(service_mqtt_optional_send(send,"topic",o,true,&scope)==-1&&tags==2&&!sends);clean();
 assert(service_mqtt_json_begin(&scope,&owner));deny_alloc=true;assert(!json_object_new_object());deny_alloc=false;
 o=json_object_new_string("invalid encoder");o->_to_json_string=bad_encoder;
 assert(service_mqtt_optional_send(send,"topic",o,true,&scope)==-1&&tags==3&&!sends);clean();
 assert(service_mqtt_json_begin(&scope,&owner));deny_alloc=true;assert(!json_object_new_object());deny_alloc=false;
 assert(!json_heap_calloc(SIZE_MAX,2));assert(service_mqtt_json_error(&scope,8)==-1&&tags==4);clean();
 /* An own failed temporary allocation is sticky even when the final graph
  * happens to encode: never publish an incomplete optional attempt. */
 assert(service_mqtt_json_begin(&scope,&owner));deny_alloc=true;assert(!json_object_new_object());deny_alloc=false;
 o=json_object_new_object();json_build_object_add(o,"value",json_object_new_int(1));
 assert(service_mqtt_optional_send(send,"topic",o,true,&scope)==PH_MQTT_MORE&&!sends&&tags==4);clean();
 assert(service_mqtt_json_begin(&scope,&owner));o=json_object_new_object();json_build_object_add(o,"value",json_object_new_int(1));
 assert(service_mqtt_optional_send(send,"topic",o,true,&scope)==-17&&sends==1&&tags==4);clean();
 puts("actual allocator owner-scoped guard/NULL/foreign-thread/invalid-depth/overflow/sender cleanup PASS");
}
'''.replace('/* PRODUCTION */',functions)
        sdk=ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'
        with tempfile.TemporaryDirectory() as directory:
            tmp=Path(directory);json=copy_patched_json(sdk,tmp/'sdk')
            for p in json.glob('*.c'):p.write_text(p.read_text().replace('%ld','%d'))
            src=tmp/'fixture.c';src.write_text(fixture)
            binary=tmp/'fixture'
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT),'-I'+str(ROOT/'tests/stubs'),'-I'+str(json),str(src),str(ROOT/'common/json_owned.c'),*map(str,json.glob('*.c')),'-Wl,--wrap=malloc,--wrap=calloc','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=30)
