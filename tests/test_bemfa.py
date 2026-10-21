"""Protocol and persistent config validation against the actual patched SDK JSON."""
from pathlib import Path
import tempfile
from tests import test_shared_mqtt as shared
ROOT=shared.ROOT
from tests.sdk_json import copy_patched_json
from tests.test_status_views import function
class BemfaConfig(__import__('unittest').TestCase):
    def test_canonical_reports(self):
        source=(ROOT/'common/mqtt/bemfa.c').read_text()
        config=(ROOT/'common/mqtt/bemfa_config.c').read_text()
        body=function(source,'static int bemfa_report_one(')+'\n'+ '\n'.join(function(config,n) for n in ('unsigned bemfa_gear_speed(', 'unsigned bemfa_fan_gear('))
        fixture=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bemfa.h"
#include "common/device_status.h"
#define MQTT_SUCCESS 0
#define PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED -2
#define QOS0 0
typedef struct {unsigned qos;void *payload;size_t payloadlen;} MQTTMessage;
static struct {bemfa_config_t config;char reported[6][16];unsigned cursor,report_at;int client;} state,*worker=&state;
static unsigned report_dirty;
static device_status_t status;
static unsigned calls;static char sent[16],topic[80];
unsigned mico_rtos_get_time(void){return 100;}
static bool bemfa_capture(device_status_t *s){*s=status;return true;}
static int bm_publish(int *c,const char *t,MQTTMessage *m){assert(c==&worker->client&&m->qos==QOS0);calls++;strcpy(topic,t);memcpy(sent,m->payload,m->payloadlen);sent[m->payloadlen]=0;return 0;}
PRODUCTION
static void report(unsigned i,const char *expected){worker->cursor=i;worker->report_at=0;worker->reported[i][0]=0;assert(!bemfa_report_one());assert(!strcmp(sent,expected));char t[80];sprintf(t,"%s/up",worker->config.topics[i]);assert(!strcmp(topic,t));}
int main(void){for(unsigned i=0;i<CHANNELS;i++)sprintf(worker->config.topics[i],"topic%u",i);
#ifdef BEMFA_TC1
 for(unsigned i=0;i<6;i++){status.control.tc1.states[i]=1;report(i,"on");status.control.tc1.states[i]=0;report(i,"off");}
 status.control.tc1.states[0]=-1;unsigned before=calls;worker->cursor=0;worker->report_at=0;assert(!bemfa_report_one()&&calls==before);
#else
 status.control.a1.effective_on=true;status.control.a1.effective_speed=50;report(0,"on#2");
 status.control.a1.effective_speed=0;report(0,"off");
 status.control.a1.led_visible=false;status.control.a1.brightness=255;report(1,"off");
 status.control.a1.led_visible=true;status.control.a1.brightness=1;report(1,"on#1");
 status.control.a1.brightness=128;report(1,"on#50");status.control.a1.brightness=255;report(1,"on#100");
 unsigned before=calls;worker->cursor=1;worker->report_at=0;assert(!bemfa_report_one()&&calls==before);
#endif
 puts("Bemfa canonical actual state reports and /up route PASS");}
'''.replace('PRODUCTION',body)
        with tempfile.TemporaryDirectory() as directory:
            probe=Path(directory)/'report.c';probe.write_text(fixture)
            for flags in (['-DCHANNELS=2'],['-DBEMFA_TC1=1','-DCHANNELS=6']):
                shared.SharedMQTT().compile_run([probe],[ROOT,ROOT/'tests/stubs'],flags)

    def test_product_command_mapping(self):
        source=(ROOT/'common/mqtt/bemfa.c').read_text()
        body='\n'.join(function(source,n) for n in ('static bool bemfa_current_config(', 'static void bemfa_command(', 'void bemfa_dispatch('))
        fixture=r'''
#include "mico.h"
#include "bemfa.h"
#include "json.h"
#include "../common/mqtt/service.h"
typedef struct {bool retained;void *payload;size_t payloadlen;} MQTTMessage;
typedef struct {char *cstring;} MQTTString;
typedef struct {MQTTMessage *message;MQTTString *topicName;} MessageData;
static struct {bemfa_config_t config;char reported[6][16];unsigned report_at;} worker_state,*worker=&worker_state;
#define COMMAND_SLOTS 4u
#define MQTT_READY 2
static unsigned state=MQTT_READY, command_head,command_tail,session_generation,report_dirty;
typedef struct {bemfa_command_t command;uint32_t revision,session;} queued_command;
static queued_command commands[COMMAND_SLOTS];
static bemfa_config_t live;
static mico_thread_t worker_thread=1;
static bool network=true,admit=true,failed_graph;
static unsigned calls,scope_count;
static char last_action[32];static int last_index,last_value,last_on;static bool has_mode;
void bemfa_config_get(bemfa_config_t *c){*c=live;}
bool bemfa_network_ready(void){return network;}
bool service_mqtt_json_begin(service_mqtt_json_scope *s,mico_thread_t *t){assert(t==&worker_thread);s->token=1;if(admit)scope_count++;return admit;}
void service_mqtt_json_end(service_mqtt_json_scope *s){assert(s->token==1&&scope_count==1);scope_count--;}
unsigned service_json_scope_failed(unsigned t){assert(t==1);return failed_graph;}
int json_heap_permit(size_t n){(void)n;return 1;}
json_object *service_command(json_object *q){
 calls++;strcpy(last_action,json_object_get_string(json_object_object_get(q,"action")));
 json_object *p=json_object_object_get(q,"params");
 last_index=json_object_get_int(json_object_object_get(p,"index"));
 json_object *on=json_object_object_get(p,"on");if(!on)on=json_object_object_get(p,"enabled");last_on=json_object_get_boolean(on);
 json_object *value=json_object_object_get(p,"speed");if(!value)value=json_object_object_get(p,"brightness");last_value=value?json_object_get_int(value):-1;
 has_mode=json_object_object_get(p,"mode")!=NULL;return json_object_new_object();
}
PRODUCTION
static void deliver(unsigned i,const char *text){MQTTMessage m={false,(void *)text,strlen(text)};MQTTString t={worker->config.topics[i]};MessageData d={&m,&t};bemfa_command(&d);bemfa_dispatch(&worker_thread);assert(!scope_count);}
int main(void){
#ifdef BEMFA_TC1
 bemfa_defaults(&live,"tc1","aabbccddeeff");
#else
 bemfa_defaults(&live,"a1","aabbccddeeff");
#endif
 live.enabled=1;worker->config=live;
#ifdef BEMFA_TC1
 for(unsigned i=0;i<6;i++){deliver(i,"on");assert(!strcmp(last_action,"outlet.set")&&last_index==i&&last_on&&last_value==-1);deliver(i,"off");assert(!last_on);}
#else
 const unsigned speeds[]={20,50,75,100};
 for(unsigned i=0;i<4;i++){char text[8];sprintf(text,"on#%u",i+1);deliver(0,text);assert(!strcmp(last_action,"fan.set")&&last_value==speeds[i]&&last_on&&!has_mode);}
 deliver(0,"off");assert(!last_on&&last_value==-1&&!has_mode);
 deliver(0,"on");assert(last_on&&last_value==-1&&!has_mode);
 deliver(1,"on#50");assert(!strcmp(last_action,"light.set")&&last_value==50&&last_on);
#endif
 unsigned before=calls;admit=false;deliver(0,"on");assert(calls==before&&command_head!=command_tail);admit=true;bemfa_dispatch(&worker_thread);assert(calls==before+1);before=calls;
 failed_graph=true;deliver(0,"on");assert(calls==before);failed_graph=false;
 network=false;deliver(0,"on");assert(calls==before);network=true;
 live.revision++;deliver(0,"on");assert(calls==before);worker->config=live;
 deliver(0,"on#garbage");assert(calls==before);deliver(0,"on");assert(calls==before+1);
 /* Session and revision changes between enqueue and HTTP dispatch drop work. */
 MQTTMessage m={false,(void *)"on",2};MQTTString t={worker->config.topics[0]};MessageData d={&m,&t};
 before=calls;bemfa_command(&d);session_generation++;bemfa_dispatch(&worker_thread);assert(calls==before);
 bemfa_command(&d);live.revision++;bemfa_dispatch(&worker_thread);assert(calls==before);worker->config=live;
 /* A full queue is bounded and preserves accepted FIFO commands. */
 for(unsigned i=0;i<7;i++)bemfa_command(&d);assert(command_head-command_tail==4);
 for(unsigned i=0;i<4;i++)bemfa_dispatch(&worker_thread);assert(calls==before+4&&command_head==command_tail);
 puts("Bemfa actual dispatcher: topic mapping, fan gears without mode override, graph admission, stale config and network rejection PASS");
}
'''.replace('PRODUCTION',body).replace('#include "../common/mqtt/service.h"','#include "common/mqtt/service.h"')
        with tempfile.TemporaryDirectory() as directory:
            temp=Path(directory);sdk=ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'
            json=copy_patched_json(sdk,temp/'sdk')
            for p in json.glob('*.c'):p.write_text(p.read_text().replace('%ld','%d'))
            probe=temp/'probe.c';probe.write_text(fixture)
            for flags in (['-DCHANNELS=2'],['-DBEMFA_TC1=1','-DCHANNELS=6']):
                shared.SharedMQTT().compile_run([probe,ROOT/'common/mqtt/bemfa_config.c',*json.glob('*.c')],
                    [ROOT,ROOT/'tests/host',json],flags)

    def test_tc1_append_only_migration(self):
        source=(ROOT/'TC1/main.c').read_text()
        body='\n'.join(function(source,n) for n in ('uint32_t app_config_legacy_size(', 'uint32_t app_config_legacy_next_size(', 'bool app_config_migrate_legacy('))
        with tempfile.TemporaryDirectory() as directory:
            probe=Path(directory)/'migration.c'
            probe.write_text('#include "mico.h"\n#include "mico_system.h"\n#include "TC1/main.h"\n#include "TC1/config_legacy.h"\n'+body+r'''
int main(void){user_config_t c;memset(&c,0xa5,sizeof(c));c.version=USER_CONFIG_VERSION;
 unsigned size=offsetof(user_config_t,bemfa);unsigned char before[sizeof(c)];memcpy(before,&c,sizeof(c));
 assert(app_config_legacy_size()==size);assert(app_config_migrate_legacy(&c,size));
 assert(!memcmp(before,&c,size));bemfa_config_t empty={0};assert(!memcmp(&c.bemfa,&empty,sizeof(empty)));
 assert(!app_config_migrate_legacy(&c,sizeof(c)));assert(app_config_legacy_next_size(size)==sizeof(user_config_v11_t));
 assert(app_config_legacy_next_size(sizeof(user_config_v11_t))==sizeof(user_config_v10_t));
 puts("TC1 old v12 extension migration preserves the entire original prefix PASS");}
''')
            shared.SharedMQTT().compile_run([probe],[ROOT,ROOT/'tests/host',ROOT/'tests/stubs',ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'])
    def test_protocol_and_config(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk=ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'
            json=copy_patched_json(sdk,Path(directory)/'sdk')
            for p in json.glob('*.c'):p.write_text(p.read_text().replace('%ld','%d'))
            shared.SharedMQTT().compile_run([ROOT/'tests/mqtt_core/bemfa_config.c',ROOT/'common/mqtt/bemfa_config.c',*json.glob('*.c')],
                [json,ROOT/'tests/stubs'])
