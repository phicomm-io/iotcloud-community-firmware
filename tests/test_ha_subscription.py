"""Actual product subscription plans deliver only isolated HA JSON commands."""
from pathlib import Path
import tempfile
import unittest
from tests.test_runtime_mqtt_mp import function
from tests import test_shared_mqtt as shared
ROOT=Path(__file__).resolve().parents[1]

class HaSubscription(unittest.TestCase):
 def test_product_plans_and_routes(self):
  for model in ('A1','M1'):
   with self.subTest(model=model), shared.SharedMQTT().protocol(model) as protocol:
    source=(ROOT/model/'mqtt.c').read_text()
    route='command' if model=='A1' else 'native_command'
    topic_name='ha_command_topic' if model=='A1' else 'command_topic'
    self.assertIn('mqtt_leaf(&account, "ha/command", '+topic_name, function(source,'static int session_prepare('))
    code=r'''
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <string.h>
#include "MQTTClient.h"
#include "common/mqtt/session.h"
#include "common/ha_command.h"
#define FEATURE_TOPIC_MAX 512
#define A1_JSON_LIMIT 4096
static mqtt_account_t account;
static char base[192],wildcard[512],ha_command_topic[512],command_topic[512];
static char feature_filters[4][512],sensor_filter[512]="sensor/command",co2_filter[512]="sensor/+/+",source_availability_filter[512]="source/availability";
static unsigned count,ha_calls,native_calls;
static bool session_need_availability,session_subscribed,current=true,ota;
static bool session_current(void){return current;}
static bool ota_job_active(void){return ota;}
static void feature_command(MessageData*d){(void)d;}
static void ha_birth(MessageData*d){(void)d;}
static int publish_bytes(const char*t,const void*p,size_t n,bool r){(void)t;(void)p;(void)n;(void)r;return 0;}
static bool feature_mqtt_input(const char*t,const void*p,size_t n,bool r){(void)t;(void)p;(void)n;(void)r;return false;}
static void service_mqtt_input(int(*pub)(const char*,const void*,size_t,bool),const mqtt_account_t*a,const void*p,size_t n,bool retained){
 (void)pub;(void)a;ph_value v;if(!retained&&phv_parse(1,p,n,&v))native_calls++;
}
ph_buf *service_command_mp(const void*p,size_t n){ph_value v;assert(phv_parse(1,p,n,&v));ha_calls++;return NULL;}
static void ota_command(const char*b,const void*p,size_t n,bool r){(void)b;(void)p;(void)n;(void)r;}
enum {A1_CONTROL_FAN,A1_CONTROL_LIGHT,A1_CONTROL_LOCK,A1_SET_ON=1,A1_SET_SPEED=2,A1_SET_ENABLED=8,A1_SET_ALWAYS=16};
typedef struct{unsigned kind,fields,on,enabled,always_on,speed;}a1_control_t;
bool service_control_begin(void){return true;}
void service_control_end(void){}
static bool a1_control_apply(a1_control_t*r,bool a,bool b){(void)r;(void)a;(void)b;return true;}
'''
    if model=='A1':code+=function(source,'static bool topic_is(')+'\n'
    code+=function(source,'static void '+route+'(')+'\n'+function(source,'static bool session_subscription(')
    code+=r'''
static void deliver(ph_mqtt_subscription*s,const void*p,size_t n,bool retained,bool length_topic){
 MQTTMessage m={0};m.payload=(void*)p;m.payloadlen=n;m.retained=retained;
 MQTTString topic={0};if(length_topic){topic.lenstring.data=(char*)s->filter;topic.lenstring.len=strlen(s->filter);}else topic.cstring=(char*)s->filter;
 MessageData d={&m,&topic};s->handler(&d);
}
int main(void){
 assert(mqtt_account_init(&account,1,"MODEL","alice","aabbccddeeff"));strcpy(base,account.base);
 assert(mqtt_leaf(&account,"command",wildcard,sizeof(wildcard)));
 assert(mqtt_leaf(&account,"ha/command",ha_command_topic,sizeof(ha_command_topic)));strcpy(command_topic,ha_command_topic);
 const char json[]="{\"action\":\"status.get\",\"params\":{}}";
 const unsigned char mp[]={0x81,0xa6,'a','c','t','i','o','n',0xaa,'s','t','a','t','u','s','.','g','e','t'};
 for(unsigned features=0;features<=4;features++)for(unsigned availability=0;availability<2;availability++){
  count=features;session_need_availability=availability;
  for(unsigned f=0;f<count;f++)snprintf(feature_filters[f],512,"feature/%u",f);
  ph_mqtt_subscription s,native={0},ha={0};unsigned total=0,hits=0;
  for(unsigned i=0;i<12;i++){
   if(!session_subscription(NULL,i,&s))break;total++;
   if(!strcmp(s.filter,wildcard))native=s;
   if(!strcmp(s.filter,ha_command_topic)){ha=s;hits++;assert(s.qos==QOS1);}
  }
  assert(hits==1&&native.handler&&ha.handler&&total==EXPECTED);
  unsigned h=ha_calls,n=native_calls;
  deliver(&ha,json,sizeof(json)-1,false,false);deliver(&ha,json,sizeof(json)-1,false,true);assert(ha_calls==h+2&&native_calls==n);
  deliver(&ha,json,sizeof(json)-1,true,false);deliver(&ha,mp,sizeof(mp),false,false);assert(ha_calls==h+2&&native_calls==n);
  deliver(&native,json,sizeof(json)-1,false,false);assert(ha_calls==h+2&&native_calls==n);
  deliver(&native,mp,sizeof(mp),false,true);assert(native_calls==n+1);
  deliver(&native,mp,sizeof(mp),true,false);assert(native_calls==n+1);
  current=false;deliver(&ha,json,sizeof(json)-1,false,false);current=true;
  ota=true;deliver(&ha,json,sizeof(json)-1,false,false);ota=false;assert(ha_calls==h+2);
  memset(&s,0x5a,sizeof(s));ph_mqtt_subscription before=s;
  assert(!session_subscription(NULL,UINT_MAX,&s)&&!memcmp(&before,&s,sizeof(s)));
 }
}
'''.replace('MODEL',model.lower()).replace('EXPECTED','3+features' if model=='A1' else '5+availability')
    code=code.replace('#include <limits.h>','#include <limits.h>\n#include <stdio.h>')
    with tempfile.TemporaryDirectory() as tmp:
     path=Path(tmp)/'probe.c';path.write_text(code)
     shared.SharedMQTT().compile_run([path,ROOT/'common/typed_wire.c',ROOT/'common/wire_buffer.c'],[ROOT,ROOT/'tests/mqtt_core/stubs',protocol,protocol/'mico',protocol/'MQTTPacket'])
