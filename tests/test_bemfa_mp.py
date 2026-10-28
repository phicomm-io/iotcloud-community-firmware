"""Bemfa uses POD commands and strict native MP config; no JSON library."""
from pathlib import Path
import tempfile
import subprocess
import unittest
from tests import test_sensor_producers_mp as producers
from tests.test_status_views import function
ROOT=Path(__file__).resolve().parents[1]

class BemfaMp(unittest.TestCase):
 def test_configuration_validation_and_direct_fields(self):
  source=r'''
#include "common/mqtt/bemfa.h"
#include <assert.h>
#include <string.h>
static bemfa_config_t live;static bool fail_save;
void bemfa_config_get(bemfa_config_t *out){*out=live;}
bool bemfa_config_save(const bemfa_config_t *next){if(fail_save)return false;live=*next;return true;}
#include "common/mqtt/bemfa_config.c"
static ph_buf *params(bool bad,bool duplicate){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_bool(&w,"enabled",true);
 phw_text(&w,"app_id","id");phw_text(&w,"secret_key","secret");
 if(bad)phw_int(&w,"unknown",1);
 if(duplicate){phw_begin(&w,"topics",true);phw_text(&w,NULL,"same001");phw_text(&w,NULL,"same001");for(unsigned i=2;i<6;i++)phw_text(&w,NULL,"");phw_end(&w);}
 phw_end(&w);return phw_take(&w);
}
int main(void){
 bemfa_defaults(&live,"tc1","aabbccddeeff");assert(bemfa_config_valid(&live,true));
 const char *code;ph_value v;
 for(unsigned i=0;i<3;i++){
  ph_buf *b=params(i==0,i==1);assert(phv_parse(1,b->buf,b->len,&v));bemfa_config_t before=live;
  fail_save=i==2;assert(!bemfa_config_set_mp(&v,&code));assert(!strcmp(code,i==2?"storage_error":"invalid_params"));assert(!memcmp(&live,&before,sizeof(live)));ph_buf_free(b);
 }
 fail_save=false;ph_buf *b=params(false,false);assert(phv_parse(1,b->buf,b->len,&v));assert(bemfa_config_set_mp(&v,&code));memset(b->buf,0,b->len);ph_buf_free(b);assert(live.enabled&&!strcmp(live.app_id,"id"));
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);bemfa_config_mp_fields(&w);phw_end(&w);b=phw_take(&w);assert(b&&phv_parse(1,b->buf,b->len,&v));
 ph_value field;assert(phv_get(&v,"bemfa",&field));assert(!phv_get(&field,"secret_key",&v));assert(phv_get(&field,"secret_key_set",&v)&&v.kind==PHV_TRUE);ph_buf_free(b);
 bemfa_command_t c;assert(bemfa_parse(BEMFA_FAN,"on#3",4,&c)&&c.value==75&&c.on);assert(!bemfa_parse(BEMFA_FAN,"on\0",3,&c));
 return 0;
}
'''
  # The fixture includes the actual module and enables its target branch.
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'test.c').write_text(source)
   subprocess.run(['cc','-std=gnu99','-DPHICOMM_BEMFA','-DBEMFA_TC1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-I'+str(ROOT),str(p/'test.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)

 def test_deferred_owned_pod_dispatch(self):
  text=(ROOT/'common/mqtt/bemfa.c').read_text()
  body='\n'.join(function(text,n) for n in ('static bool bemfa_current_config(', 'static void bemfa_command(', 'void bemfa_dispatch('))
  for tc1 in (False,True):
   flags='#define _POSIX_C_SOURCE 200809L\n'+('#define BEMFA_TC1\n' if tc1 else '')
   producers.SensorProducers().compile_run(flags+r'''
#include "common/mqtt/bemfa.h"
#include "common/automation.h"
#include <assert.h>
#include <string.h>
typedef struct{bool retained;void *payload;size_t payloadlen;}MQTTMessage;
typedef struct{char *cstring;}MQTTString;
typedef struct{MQTTMessage *message;MQTTString *topicName;}MessageData;
static struct{bemfa_config_t config;}data,*worker=&data;
#define COMMAND_SLOTS 4u
#define MQTT_READY 2
#ifdef BEMFA_TC1
#define CHANNELS 6
#else
#define CHANNELS 2
#endif
static unsigned state=MQTT_READY,command_head,command_tail,session_generation,report_dirty;
typedef struct{bemfa_command_t command;uint32_t revision,session;}queued_command;
static queued_command commands[4];static bemfa_config_t live;static bool network=true;
static unsigned calls;static automation_action_t last;
void bemfa_config_get(bemfa_config_t *out){*out=live;}
bool bemfa_network_ready(void){return network;}
static bool apply(const automation_action_t *a,bool button){assert(!button);last=*a;calls++;return true;}
bool a1_automation_execute(const automation_action_t*a,bool b){return apply(a,b);}
bool tc1_automation_execute(const automation_action_t*a,bool b){return apply(a,b);}
#include "common/mqtt/bemfa_config.c"
''' + body+r'''
static void deliver(unsigned i,char *payload){MQTTMessage m={false,payload,strlen(payload)};MQTTString t={worker->config.topics[i]};MessageData d={&m,&t};bemfa_command(&d);memset(payload,0,strlen(payload));bemfa_dispatch(NULL);}
int main(void){
#ifdef BEMFA_TC1
 bemfa_defaults(&live,"tc1","aabbccddeeff");
#else
 bemfa_defaults(&live,"a1","aabbccddeeff");
#endif
 live.enabled=1;worker->config=live;
 char bytes[8]="on";deliver(0,bytes);assert(calls==1&&last.value==1);
#ifdef BEMFA_TC1
 assert(last.kind==AUTO_OUTLET&&last.target==0);
 for(unsigned i=0;i<6;i++){strcpy(bytes,"on");deliver(i,bytes);assert(last.kind==AUTO_OUTLET&&last.target==i&&last.value);strcpy(bytes,"off");deliver(i,bytes);assert(!last.value);}
#else
 assert(last.kind==AUTO_FAN&&last.speed==0);
 const unsigned speeds[]={20,50,75,100};
 for(unsigned i=0;i<4;i++){snprintf(bytes,sizeof(bytes),"on#%u",i+1);deliver(0,bytes);assert(last.kind==AUTO_FAN&&last.speed==speeds[i]&&last.value&&!last.reserved);}
 strcpy(bytes,"off");deliver(0,bytes);assert(!last.value&&last.speed==0);
 strcpy(bytes,"on#50");deliver(1,bytes);assert(last.kind==AUTO_LIGHT&&last.value&&last.speed==50);
 strcpy(bytes,"on");deliver(1,bytes);assert(last.kind==AUTO_LIGHT&&last.speed==255);
#endif
 unsigned before=calls;strcpy(bytes,"on#bad");deliver(0,bytes);assert(calls==before);network=false;strcpy(bytes,"on");deliver(0,bytes);assert(calls==before);network=true;
 MQTTMessage m={false,"off",3};MQTTString t={worker->config.topics[0]};MessageData d={&m,&t};
 bemfa_command(&d);session_generation++;bemfa_dispatch(NULL);assert(calls==before);
 bemfa_command(&d);live.revision++;bemfa_dispatch(NULL);assert(calls==before);worker->config=live;
 for(unsigned i=0;i<7;i++)bemfa_command(&d);assert(command_head-command_tail==4);for(unsigned i=0;i<4;i++)bemfa_dispatch(NULL);assert(calls==before+4);
 return 0;
}
''')
