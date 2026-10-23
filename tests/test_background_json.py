"""Real virtual/discovery producers: byte parity and graph lifetime at publish."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.sdk_json import copy_patched_json
from tests.test_http_json_budget import function

ROOT = Path(__file__).resolve().parents[1]

class BackgroundJson(unittest.TestCase):
    def test_a1_native_discovery_callbacks(self): self.check_native_discovery_callbacks('A1')
    def test_m1_native_discovery_callbacks(self): self.check_native_discovery_callbacks('M1')
    def test_a1_fresh_discovery_callbacks(self): self.check_native_discovery_callbacks('A1', fresh=True)
    def check_native_discovery_callbacks(self, model, fresh=False):

        with self.subTest(model=model):
            source=(ROOT/model/'mqtt.c').read_text()
            service=(ROOT/'common/mqtt/service.c').read_text()
            generic=(ROOT/'common/service.c').read_text()
            functions='static struct {unsigned token,failed;void *owner;} json_scope;\n'+'\n'.join(function(generic,n) for n in
                ('void json_heap_failure(', 'bool service_json_scope_begin(', 'unsigned service_json_scope_failed(', 'unsigned service_json_scope_end('))+'\n'
            functions+='\n'.join(function(service,n) for n in
                ('bool service_mqtt_json_begin(', 'static unsigned service_mqtt_json_finish(', 'void service_mqtt_json_end(', 'int service_mqtt_json_error(', 'int service_mqtt_optional_send('))+'\n'
            functions+=function(source,'static void str(')+'\n'
            if model=='A1':
                functions+=function(source,'static void topic(')+'\n'
                functions+=function(source,'static __attribute__((noinline)) int filter_discovery(')+'\n'
                functions+='static int discovery_build(const a1_config_t *,unsigned,service_mqtt_json_scope *);\n'+function(source,'static int fresh_discovery(')+'\n'
            if model=='M1': functions+=source[source.index('enum { MQTT_BRIGHTNESS_INDEX'):source.index(';',source.index('enum { MQTT_BRIGHTNESS_INDEX'))+1]+'\n'
            functions+='static bool discovery_deferred;\nstatic uint32_t discovery_retry_at;\n'+function(source,'static int discovery_build(const '+model.lower()+'_config_t *c,')+'\n'+function(source,'static int discovery(')
            fixture=r'''
#include "common/json_owned.h"
#include "common/mqtt_account.h"
#include "json_heap.h"
#include "printbuf.h"
#include "PLATFORM_HEADER"
#define MQTT_SUCCESS 0
#define MQTT_FAILURE -1
#define PH_MQTT_MORE 2
static mico_thread_t mqtt_owner_thread=1;
typedef struct {unsigned token,claim;}service_mqtt_json_scope;
static bool token_active,claim_active,ready=true;static unsigned json_owner,service_lock;
void service_mqtt_json_end(service_mqtt_json_scope *);
static unsigned service_json_claim(void){assert(!token_active);token_active=true;json_owner=1;return 1;}
static unsigned service_memory_claim(size_t n){(void)n;assert(!claim_active);claim_active=true;return 1;}
static void service_json_release(unsigned t){assert(t&&token_active);token_active=false;json_owner=0;}
static void service_memory_release(unsigned t){assert(t&&claim_active);claim_active=false;}
#define PH_MQTT_ERROR_APP_DISCOVERY_GRAPH 8
#define PH_MQTT_ERROR_APP_DISCOVERY_IDENTITY 9
#define PH_MQTT_ERROR_APP_DISCOVERY_ENCODING 10
static int ph_mqtt_app_error(unsigned stage,int rc){assert(stage>=8 && stage<=10 && rc<0);return rc;}
#define A1_VERSION "fixture"
#define M1_VERSION "fixture"
#define A1_FILTER_MAX_HOURS 20000u
uint32_t test_now;
static unsigned allocation,fail_at,callbacks;static bool quiet;static int callback_rc;
int json_heap_permit(size_t n){(void)n;return !fail_at||++allocation!=fail_at;}
static mqtt_account_t account;static char base[192],availability_topic[MQTT_TOPIC_MAX],command_topic[MQTT_TOPIC_MAX];
#ifdef MODEL_A1
bool feature_bound(void){return false;}
bool feature_ha_device(char*i,size_t n,char*t,size_t m,bool*g){(void)i;(void)n;(void)t;(void)m;(void)g;return false;}
#else
typedef struct{char name[64];}m1_config_t;
mico_mutex_t m1_lock;m1_state_t m1_state;
#endif
static int publish(const char*t,const char*b,bool r){
 assert(t&&b&&r&&!token_active&&!claim_active);json_heap_stats_t s;json_heap_snapshot(&s);
#ifndef LEGACY_PUBLISH
 assert(s.live_blocks==2);
#endif
 unsigned saved=fail_at;fail_at=0;
 json_object *parsed=json_tokener_parse(b);assert(parsed);json_object_put(parsed);fail_at=saved;
 if(!quiet)printf("%u %s %u %s\n",callbacks,t,r,b);callbacks++;return callback_rc;
}
#ifdef LEGACY_PUBLISH
static int legacy_publish(int(*p)(const char*,const char*,bool),const char*t,json_object*o,bool r,service_mqtt_json_scope *scope){
 const char*w=json_object_to_json_string(o);assert(w);service_mqtt_json_end(scope);int rc=p(t,w,r);json_object_put(o);return rc;
}
#define discovery_sender legacy_publish
#else
#define discovery_sender service_mqtt_optional_send
#endif
/* PRODUCTION_FUNCTIONS */
static int discovery_all(MODEL_CONFIG*c){
 for(unsigned i=0;i<EXPECTED_CALLBACKS;i++){
  test_now+=1000u;unsigned before=callbacks;int rc=discovery(c,i);assert(callbacks-before<=1);if(rc)return rc;
 }return 0;
}
static void empty(void){json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks);}
int main(void){
 assert(mqtt_account_init(&account,1,"MODEL_LOWER","test","aabbccddee01"));strcpy(base,account.base);
 assert(mqtt_leaf(&account,"availability",availability_topic,sizeof(availability_topic)));
 assert(mqtt_leaf(&account,"command",command_topic,sizeof(command_topic)));
 MODEL_CONFIG c={.name="Room / \\\" UTF8 中文"};
#ifndef MODEL_A1
 m1_state.scd40.received=true;m1_state.barometer.received=true;
#endif
 assert(discovery_all(&c)==0);assert(callbacks==EXPECTED_CALLBACKS);empty();
 quiet=true;callbacks=0;callback_rc=-17;assert(discovery_all(&c)==-17);assert(callbacks==1);empty();
#ifndef LEGACY_PUBLISH
 callback_rc=0;unsigned failures=0;
 for(unsigned i=1;i<10000;i++){
  allocation=callbacks=0;fail_at=i;int rc=discovery_all(&c);fail_at=0;empty();
  if(!rc){assert(callbacks==EXPECTED_CALLBACKS);break;}
  assert(rc==PH_MQTT_MORE);failures++;assert(i<9999);
 }
 assert(failures>100);
#endif
 return 0;
}
'''
            functions=functions.replace('int rc = service_mqtt_optional_send(', 'int rc = discovery_sender(').replace('return service_mqtt_optional_send(', 'return discovery_sender(')
            fixture=fixture.replace('/* PRODUCTION_FUNCTIONS */',functions)
            fixture=fixture.replace('PLATFORM_HEADER',model.lower()+'.h').replace('MODEL_LOWER',model.lower()).replace('MODEL_CONFIG',model.lower()+'_config_t').replace('EXPECTED_CALLBACKS','12' if fresh else '10' if model=='A1' else '7')
            sdk=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
            with tempfile.TemporaryDirectory() as directory:
                temp=Path(directory);json=copy_patched_json(sdk,temp/'sdk')
                for path in json.glob('*.c'):path.write_text(path.read_text().replace('%ld','%d'))
                src=temp/'probe.c';src.write_text(fixture);results=[]
                for legacy in (True,False):
                    binary=temp/('legacy' if legacy else 'owned')
                    flags=(['-DLEGACY_PUBLISH'] if legacy else []) + (['-DA1_FRESH_AIR'] if fresh else [])
                    subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g','-DMODEL_'+model,
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',*flags,
                        '-I'+str(ROOT),'-I'+str(ROOT/'tests/stubs'),'-I'+str(json),str(src),
                        str(ROOT/'common/json_owned.c'),*map(str,json.glob('*.c')),'-lm','-o',str(binary)],check=True)
                    results.append(subprocess.run([str(binary)],check=True,stdout=subprocess.PIPE,timeout=60).stdout)
                self.assertEqual(results[0],results[1])

    def test_virtual_production_callbacks(self):
        # Include the existing platform shim, whose runtime.c include is the real
        # complete production translation unit; its unrelated scenarios are idle.
        fixture = r'''
#include "common/json_owned.h"
#include "json_heap.h"
#include "printbuf.h"
#ifdef LEGACY_PUBLISH
static int legacy_publish(int(*p)(const char*,const char*,bool),const char*t,json_object*o,bool r){
 const char *wire=o?json_object_to_json_string(o):NULL;
 int rc=wire?p(t,wire,r):-1;json_object_put(o);return rc;
}
#define json_owned_publish legacy_publish
#endif
#include "mico.h"
static uint32_t graph_test_time(void);
#define mico_rtos_get_time graph_test_time
#include "common/mqtt/service.h"
#define RUNTIME_EXTERNAL_MQTT_SCOPE
#define MQTT_FAILURE -1
#define main unused_runtime_scenarios
#include "tests/test_runtime.c"
#undef main
static mico_thread_t mqtt_owner_handle=1;
mico_thread_t *ph_mqtt_owner_thread(void){return &mqtt_owner_handle;}
static int ph_mqtt_app_error(unsigned stage,int rc){(void)stage;return rc;}
/* OPTIONAL_SCOPE_FUNCTIONS */
static bool update_on_clock;
static unsigned clock_reads;
static uint32_t graph_test_time(void){
 if(update_on_clock&&++clock_reads==2){
  update_on_clock=false;test_now+=1500;
  samples[0].received_ms=test_now;samples[0].co2_received_ms=test_now;
 }
 return test_now;
}
static unsigned graph_callbacks,wire_callbacks,callbacks,averages,sensors,blocked_token;
static bool hold_after_average,update_during_leaf,check_fresh_graph,slow_ack;
static unsigned leaf_seen;
static unsigned quota,turn_quota=4,max_turn_outputs;
static void turn(void);
static void drain(void);
static int output(const char*t,const char*b,bool r){
 assert(t&&b);if(!quota)return 2;--quota;
 if(slow_ack){
  test_now+=1500;
  const char *leaves[]={"/fan/state","/fan/mode/state","/fan/percentage/state",
   "/light/state","/child_lock/state","/led_always_on/state","/availability"};
  for(unsigned i=0;i<7;i++){
   size_t n=strlen(leaves[i]),length=strlen(t);
   if(length>=n&&!strcmp(t+length-n,leaves[i]))leaf_seen|=1u<<i;
  }
 }

 if(update_during_leaf&&strstr(t,"/fan/state")){
  update_during_leaf=false;test_now+=1500;
  samples[0].received_ms=test_now;samples[0].co2_received_ms=test_now;
 }
 if(check_fresh_graph&&(strstr(t,"/average")||strstr(t,"/sensor"))&&!r){
  json_object *o=json_tokener_parse(b);assert(o);
  assert(json_object_get_boolean(json_object_object_get(o,"co2_valid")));
  assert(json_object_get_int(json_object_object_get(o,"age_ms"))<10000);
  json_object_put(o);
 }
json_heap_stats_t s;json_heap_snapshot(&s);
 unsigned token=service_json_claim();assert(token);service_json_release(token);
 if(strstr(t,"/average")){averages++;if(hold_after_average){hold_after_average=false;blocked_token=service_json_claim();assert(blocked_token);}}
 if(strstr(t,"/sensor")&&!r)sensors++;
#ifndef LEGACY_PUBLISH
 assert(s.live_blocks==0||s.live_blocks==2);
#endif
 if(s.live_blocks>2)graph_callbacks++;if(s.live_blocks==2)wire_callbacks++;
 printf("%u %s %u %s\n",callbacks++,t,r,b);return 0;
}
static void turn(void){quota=turn_quota;unsigned before=callbacks;
 int rc=feature_mqtt_service(output,true);assert(rc==0||rc==2);assert(callbacks-before<=turn_quota);if(callbacks-before>max_turn_outputs)max_turn_outputs=callbacks-before;
}
static void drain(void){for(unsigned i=0;i<300;i++){
 turn();if(blocked_token)return;
 if(mqtt_online&&!mqtt_discovery_pending&&!mqtt_leaf_pending)return;
 test_now+=100;
 }assert(!"publication did not make bounded progress");}
int main(void){
 service_init();feature_defaults(&stored);assert(!feature_start());
 strcpy(stored.group,"room");stored.peer_count=1;
 strcpy(stored.peers[0].id,"112233445566");stored.peers[0].confirmed=true;
 configuration=stored;test_now=6000;
 samples[0]=(feature_sample_t){.valid=true,.sequence=1,.received_ms=6000,
  .value={22,45,100,.05},.co2_supported=true,.co2_valid=true,
  .co2=1100,.co2_received_ms=6000};
 drain();
 test_now=12000;samples[0].received_ms=test_now;samples[0].co2_received_ms=test_now;
 drain();assert(callbacks>10);
 unsigned saved_callbacks=callbacks,held=service_json_claim();assert(held);
 uint64_t failures=service_memory_failures();test_now=18000;
 drain();assert(callbacks==saved_callbacks&&service_memory_failures()==failures);
 service_json_release(held);drain();
 assert(callbacks>saved_callbacks);
 assert(averages==0&&sensors==0);
 unsigned held_telemetry=service_json_claim();assert(held_telemetry);
 saved_callbacks=callbacks;test_now=24000;turn();assert(callbacks==saved_callbacks);
 service_json_release(held_telemetry);drain();assert(callbacks>saved_callbacks);
 /* Lose the coordinator role after discovery began but before it finishes.
  * Readiness must not retain an impossible local pending batch. */
 configuration.member_count=1;strcpy(configuration.members[0].id,"000000000001");
 configuration.revision++;stored=configuration;test_now=25000;member_seen[0]=0;
 turn();assert(mqtt_discovery_pending);
 member_seen[0]=test_now;turn();
 assert(!feature_is_coordinator()&&!mqtt_discovery_pending&&feature_mqtt_initial_ready());
 member_seen[0]=0;turn();assert(mqtt_discovery_pending);
 drain();assert(!mqtt_discovery_pending&&feature_mqtt_initial_ready());
 /* Valid slow ACKs take more than one 5s production interval per turn.
  * Replenishing the latest data must not restart a fixed leaf prefix forever. */
 slow_ack=true;leaf_seen=0;max_turn_outputs=0;
 mqtt_tick=test_now-5000;
 for(unsigned i=0;i<60;i++){turn();test_now+=50;}
 assert(max_turn_outputs==4);
 assert(leaf_seen==127&&averages==0&&sensors==0);
 /* Physical publications may leave only one or two global slots. Quota
  * rejection must resume the rejected virtual item rather than skip it. */
 for(turn_quota=1;turn_quota<=2;turn_quota++){
  leaf_seen=0;
  for(unsigned i=0;i<100;i++){turn();test_now+=50;}
  assert(leaf_seen==127&&averages==0&&sensors==0);
 }
 turn_quota=4;slow_ack=false;
 free(published_layout);published_layout=NULL;
 json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks);
#ifdef LEGACY_PUBLISH
 assert(graph_callbacks>0);
#else
 assert(wire_callbacks>0);
#endif
 return 0;
}
'''
        mqtt=(ROOT/'common/mqtt/service.c').read_text()
        optional='\n'.join(function(mqtt,n) for n in ('bool service_mqtt_json_begin(', 'static unsigned service_mqtt_json_finish(', 'void service_mqtt_json_end(', 'int service_mqtt_json_error('))
        optional+='\n#ifdef LEGACY_PUBLISH\n'+r'''
int service_mqtt_optional_send(int(*p)(const char*,const char*,bool),const char*t,json_object*o,bool r,service_mqtt_json_scope *scope){
 const char *wire=o?json_object_to_json_string(o):NULL;service_mqtt_json_end(scope);
 int rc=wire?p(t,wire,r):-1;json_object_put(o);return rc;
}
'''+ '\n#else\n'+function(mqtt,'int service_mqtt_optional_send(')+'\n#endif\n'
        fixture=fixture.replace('/* OPTIONAL_SCOPE_FUNCTIONS */',optional)
        sdk = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        with tempfile.TemporaryDirectory() as directory:
            temp=Path(directory);json=copy_patched_json(sdk,temp/'sdk')
            for p in json.glob('*.c'):
                p.write_text(p.read_text().replace('%ld','%d'))
            source=temp/'probe.c';source.write_text(fixture)
            results=[]
            for legacy in (True,False):
                binary=temp/('legacy' if legacy else 'owned')
                flags=['-DLEGACY_PUBLISH'] if legacy else []
                bear=ROOT/'build/ota-tls/bearssl-0.6'
                subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g',
                    '-DFEATURE_DEVICE_A1','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-no-pie',*flags,'-I'+str(ROOT),
                    '-I'+str(ROOT/'tests/stubs'),'-I'+str(json),str(source),
                    '-I'+str(bear/'inc'),'-I'+str(bear/'src'),
                    *[str(bear/'src'/p) for p in ('hash/sha2small.c','codec/dec32be.c','codec/enc32be.c','codec/enc64be.c')],
                    str(ROOT/'common/service.c'),str(ROOT/'common/json_owned.c'),
                    *map(str,json.glob('*.c')),'-lm','-o',str(binary)],check=True)
                results.append(subprocess.run([str(binary)],check=True,
                    stdout=subprocess.PIPE,timeout=30).stdout)
            self.assertEqual(results[0],results[1])

if __name__=='__main__':unittest.main()
