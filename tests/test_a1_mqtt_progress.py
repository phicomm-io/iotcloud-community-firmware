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
        scheduler = function(source, 'static int states_step(')
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
typedef struct {int unused;} a1_config_t;
typedef struct {int unused;} mqtt_state_t;
#define MQTT_SUCCESS 0
static unsigned state_index,state_pending,state_seen,seen[7],calls;
static bool sent,fail;
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
 assert(sent&&!state_pending);return 0;
}
'''.replace('/* PRODUCTION */',scheduler)
        with tempfile.TemporaryDirectory() as d:
            c=Path(d)/'probe.c';c.write_text(fixture);binary=Path(d)/'probe'
            subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=10)
