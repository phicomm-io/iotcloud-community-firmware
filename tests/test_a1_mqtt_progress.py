"""Execute the production A1 state batch scheduler without sockets."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_http_json_budget import function
ROOT = Path(__file__).resolve().parents[1]
class A1Progress(unittest.TestCase):
    def test_state_batches_survive_edges_and_failures(self):
        source = (ROOT/'A1/mqtt.c').read_text()
        scheduler = function(source, 'static int states_step(')+'\n'+function(source, 'static bool filter_priority(')
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
typedef struct {int unused;} a1_config_t;
typedef struct {int unused;} mqtt_state_t;
#define MQTT_SUCCESS 0
static unsigned state_index,state_pending,state_seen,seen[7],calls;
static bool sent,fail,filter_turn;
static int states(const a1_config_t*c,const mqtt_state_t*s,unsigned i){
 (void)c;(void)s;assert(i<7);calls++;if(fail)return -1;seen[i]++;return 0;
}
/* PRODUCTION */
int main(void){
 a1_config_t c={0};mqtt_state_t s={0};bool snapshot_sent;
 /* One revision edge must complete all leaves on later quiet turns. */
 assert(!states_step(&c,&s,true,&snapshot_sent));assert(!sent&&!snapshot_sent);
 for(unsigned i=0;i<6;i++)assert(!states_step(&c,&s,false,&snapshot_sent));
 assert(sent&&!state_pending&&snapshot_sent);for(unsigned i=0;i<7;i++)assert(seen[i]==1);
 unsigned before=calls;assert(!states_step(&c,&s,false,&snapshot_sent)&&calls==before);
 /* Failed sends retain their bit/cursor; a retry executes it once. */
 fail=true;unsigned cursor=state_index;assert(states_step(&c,&s,true,&snapshot_sent)==-1);
 assert(state_pending==127&&state_index==cursor);fail=false;
 for(unsigned i=0;i<7;i++)assert(!states_step(&c,&s,false,&snapshot_sent));
 assert(!state_pending&&sent);for(unsigned i=0;i<7;i++)assert(seen[i]==2);
 /* Continuous updates cannot starve leaves or initial online readiness. */
 state_seen=0;sent=false;
 for(unsigned i=0;i<21;i++){
  assert(!states_step(&c,&s,true,&snapshot_sent));
  if(i>=6)assert(sent);
 }
 for(unsigned i=0;i<7;i++)assert(seen[i]==5);
 for(unsigned i=0;i<7;i++)assert(!states_step(&c,&s,false,&snapshot_sent));
 assert(sent&&!state_pending);
 /* Continuous urgent filter changes must not starve six ordinary leaves or
  * first READY coverage. Execute the production choice plus leaf scheduler. */
 state_seen=0;sent=false;state_index=0;filter_turn=false;
 unsigned full=0;for(unsigned i=0;i<28;i++){
  state_pending|=127;
  if(filter_priority(true)){
   full++;state_pending&=~64u;state_seen|=64u;sent=state_seen==127;
  }else assert(!states_step(&c,&s,false,&snapshot_sent));
  if(i>=13)assert(sent);
 }
 assert(full==14);for(unsigned i=0;i<6;i++)assert(seen[i]>=7);
 return 0;
}
'''.replace('/* PRODUCTION */',scheduler)
        with tempfile.TemporaryDirectory() as d:
            c=Path(d)/'probe.c';c.write_text(fixture);binary=Path(d)/'probe'
            subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=10)

class FeatureRevision(unittest.TestCase):
    def test_a1_authoritative_revision_without_snapshot_allocation(self):
        self.check_authoritative_revision('A1','a1_config_lock','a1_config')
    def check_authoritative_revision(self, model, lock, config):

        with self.subTest(model=model):
            main = (ROOT/model/'main.c').read_text()
            getter = function(main, 'uint32_t feature_config_revision(')
            mqtt = (ROOT/model/'mqtt.c').read_text()
            cycle = function(mqtt, 'static int session_cycle(')
            start = cycle.index('mqtt_state_get(&state);')
            end = cycle.index('if(discovery_index<')
            # Include the real connection/revision/filter decision, excluding
            # unrelated birth scheduling and publication after that decision.
            decision_end = cycle.index('return PH_MQTT_RECONFIGURE;', cycle.index('feature_mqtt_filters_current',start))+len('return PH_MQTT_RECONFIGURE;')
            prefix = cycle[start:decision_end]
            self.assertLess(start, end)
            fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define PH_MQTT_RECONFIGURE 1
static int held,locks,allocations;
static void mico_rtos_lock_mutex(int *p){assert(!held&&*p==0);held=1;locks++;}
static void mico_rtos_unlock_mutex(int *p){assert(held&&*p==0);held=0;}
typedef struct {struct {uint32_t revision;} features;} app_storage_t;
static app_storage_t storage;
static int __LOCK__;
static void *__CONFIG__=&storage;
/* GETTER */
typedef struct {uint32_t revision;} feature_config_t;
static void feature_config_get(feature_config_t *p){p->revision=storage.features.revision;}
static unsigned feature_revision,feature_filters[1],count;
static struct {bool wifi_connected;} state;
static unsigned cfg,current;
static bool connection=true,filters=true,changed,rediscover;
static uint8_t discovery_language;
static uint8_t device_language_get(void){return 0;}
static void mqtt_state_get(void *p){(void)p;state.wifi_connected=true;}
static void MODEL_config_get(void *p){(void)p;}
static bool same_connection(void *a,void *b){(void)a;(void)b;return connection;}
static bool feature_mqtt_filters_current(void *p,unsigned n){(void)p;(void)n;return filters;}
/* Any attempted allocation fails. The former snapshot path would reconfigure. */
static void *failed_malloc(size_t n){(void)n;allocations++;return NULL;}
#define malloc failed_malloc
static int check_cycle(void){
/* PREFIX */
changed=feature_changed;return 0;
}
int main(void){
 storage.features.revision=7;assert(feature_config_revision()==7&&locks==1&&!held);
 feature_revision=7;assert(!check_cycle()&&!changed&&allocations==0);
 storage.features.revision=8;assert(!check_cycle()&&changed&&feature_revision==8);
 assert(!check_cycle()&&!changed);
 /* Persist failure restores the authoritative value under the same mutex. */
 mico_rtos_lock_mutex(&__LOCK__);storage.features.revision=9;storage.features.revision=8;
 mico_rtos_unlock_mutex(&__LOCK__);assert(feature_config_revision()==8);
 assert(!check_cycle()&&!changed);
 storage.features.revision=9;filters=false;
 assert(check_cycle()==PH_MQTT_RECONFIGURE&&feature_revision==9);
 filters=true;connection=false;assert(check_cycle()==PH_MQTT_RECONFIGURE);
 assert(!held&&allocations==0);return 0;
}
'''.replace('/* GETTER */', getter).replace('/* PREFIX */', prefix)
            fixture=fixture.replace('__LOCK__',lock).replace('__CONFIG__',config).replace('MODEL',model.lower())
            with tempfile.TemporaryDirectory() as d:
                c=Path(d)/'probe.c';c.write_text(fixture);binary=Path(d)/'probe'
                subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra',
                    '-Wno-unused-function','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(c),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True,timeout=10)
                # Restore the former allocating decision: forced OOM must
                # fail the healthy-cycle assertion, not silently pass.
                old_prefix=prefix.replace('uint32_t current_feature_revision=feature_config_revision();',
                    'feature_config_t *fc=malloc(sizeof(*fc)); if(!fc)return PH_MQTT_RECONFIGURE; feature_config_get(fc); uint32_t current_feature_revision=fc->revision; free(fc);')
                negative=fixture.replace(prefix,old_prefix)
                self.assertNotEqual(negative,fixture)
                c.write_text(negative)
                subprocess.run([os.environ.get('CC','cc'),'-std=c99',str(c),'-o',str(binary)],check=True)
                run=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
                self.assertNotEqual(run.returncode,0,'old heap-refusal path must fail')
            prepare=function(mqtt,'static int session_prepare(')
            self.assertIn('feature_revision=feature_config_revision();',prepare)
