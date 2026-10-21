"""Execute production discovery dispatch with fake publishers/control hooks; no sockets."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_http_json_budget import function

ROOT = Path(__file__).resolve().parents[1]


class BirthProgress(unittest.TestCase):
    def test_a1_m1_birth_finishes_current_batch_before_replay(self):
        for model, count, tail in [('A1', 10, '            bool on;'),
                                   ('M1', 6, '            if(initial_index<2)')]:
            with self.subTest(model=model):
                source = (ROOT/model/'mqtt.c').read_text()
                cycle = function(source, 'static int session_cycle(')
                # Keep the unmodified production entry, admission, config and
                # discovery path. Later state encoding is a separate test.
                self.assertIn(tail, cycle)
                cycle = cycle[:cycle.index(tail)] + 'return rc;\n}'
                fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#define MQTT_SUCCESS 0
#define PH_MQTT_RECONFIGURE -3
#define PH_MQTT_MORE 2
#define BATCH /* COUNT */
typedef struct {unsigned revision;} feature_config_t;
typedef struct {unsigned revision;} a1_config_t;
typedef a1_config_t m1_config_t;
static a1_config_t cfg,current;
static struct {bool wifi_connected;struct {bool received;}scd40;} state;
static bool rediscover,sent,old_on,old_visible,filter_turn,co2_discovered;
static unsigned feature_revision,discovery_index,state_index,state_pending,state_seen;
static int old_brightness,old_fresh,old_co2_fresh;
static unsigned sequence,co2_sequence,count,snapshot_at;
static char base[8],feature_filters[4][32];static int account,publish;
static unsigned calls,resets,trace[40],inject_at;static bool injected,fail_once;
static void ota_job_poll(void){}
static bool ota_job_active(void){return false;}
static int ota_service(const char*x){(void)x;return 0;}
static unsigned mico_rtos_get_time(void){return 0;}
static int service_mqtt_snapshot(int x,int*y){(void)x;(void)y;return 0;}
static void mqtt_state_get(void*x){(void)x;state.wifi_connected=true;}
static void a1_config_get(a1_config_t*x){*x=cfg;}
static void m1_config_get(m1_config_t*x){*x=cfg;}
static bool same_connection(const a1_config_t*x,const a1_config_t*y){(void)x;(void)y;return true;}
static void feature_config_get(feature_config_t*x){x->revision=feature_revision;}
static bool feature_mqtt_filters_current(void*x,unsigned n){(void)x;(void)n;return true;}
static int feature_mqtt_service(int x,bool connected){(void)x;assert(!connected);resets++;return 0;}
static int discovery(const a1_config_t*x,unsigned index){
 (void)x;assert(index<BATCH);
 if(!injected&&index==inject_at){rediscover=true;injected=true;}
 if(fail_once&&index==inject_at){fail_once=false;return PH_MQTT_MORE;}
 assert(calls<40);trace[calls++]=index;return MQTT_SUCCESS;
}
/* PRODUCTION */
int main(void){
 for(unsigned trial=0;trial<3;trial++){
  calls=resets=discovery_index=0;injected=false;inject_at=trial?BATCH/2:0;
  rediscover=trial==0;fail_once=trial==2;
  if(!trial)injected=true;
  for(unsigned turn=0;turn<2*BATCH+4;turn++){
   int rc=session_cycle(NULL);assert(rc==0||rc==PH_MQTT_MORE);
   if(calls==2*BATCH&&!rediscover)break;
  }
  assert(calls==2*BATCH&&resets==1&&!rediscover);
  for(unsigned i=0;i<2*BATCH;i++)assert(trace[i]==i%BATCH);
 }
 return 0;
}
'''.replace('/* COUNT */', str(count)).replace('/* PRODUCTION */', cycle)
                self.compile_run(fixture)

    def test_tc1_birth_finishes_snapshot_batch_before_replay(self):
        cycle = function((ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text(),
                         'static int session_cycle(')
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define MQTT_SUCCESS 0
#define PH_MQTT_RECONFIGURE -3
#define PH_MQTT_MORE 2
#define service_sender 0
static struct {bool mode;char base[8];}account;
static bool rediscover,power_turn,power_pending,injected,fail_once;
static unsigned discovery_index,state_index,publication_cursor,snapshot_at;
static unsigned calls,trace[64],inject_at;
static bool session_current(void){return true;}
static bool ota_job_active(void){return false;}
static int ota_service(const char*x){(void)x;return 0;}
static unsigned mico_rtos_get_time(void){return 0;}
static bool power_requested(void){return false;}
static int service_mqtt_snapshot(int sender,void*x){(void)sender;(void)x;return 0;}
static int snapshot_step(void){
 unsigned index=discovery_index<12?discovery_index:12+state_index;
 assert(index<22);
 if(!injected&&index==inject_at){rediscover=true;injected=true;}
 if(fail_once&&index==inject_at){fail_once=false;return PH_MQTT_MORE;}
 assert(calls<64);trace[calls++]=index;
 if(discovery_index<12)discovery_index++;else state_index++;
 return 0;
}
/* PRODUCTION */
int main(void){
 for(unsigned trial=0;trial<3;trial++){
  calls=discovery_index=state_index=publication_cursor=0;
  injected=false;inject_at=trial==1?5:15;rediscover=trial==0;
  if(!trial)injected=true;fail_once=trial==2;
  for(unsigned turn=0;turn<60;turn++){
   int rc=session_cycle(0);assert(rc==0||rc==PH_MQTT_MORE);
   if(calls==44&&!rediscover)break;
  }
  assert(calls==44&&!rediscover);
  for(unsigned i=0;i<44;i++)assert(trace[i]==i%22);
 }
 return 0;
}
'''.replace('/* PRODUCTION */', cycle)
        self.compile_run(fixture)

    def compile_run(self, fixture):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            source = path/'probe.c'
            source.write_text(fixture)
            binary = path/'probe'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-g',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            str(source), '-o', str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=10)
