"""HA/account behaviors retained from the former DOM runtime harness.

Execute the production filter and HA discovery functions with native structs;
Python JSON is an output oracle only. No SDK DOM is linked into these probes.
"""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest
from tests.test_runtime_mqtt_mp import function

ROOT=Path(__file__).resolve().parents[1]
PRELUDE=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common/phicomm_features.h"
#include "common/ha_json.h"
#define LOCAL_ID "aabbccddeeff"
#define PH_MQTT_MORE 1
#define PH_MQTT_ERROR_APP_DISCOVERY_GRAPH -20
static feature_config_t config;
static mqtt_account_t account;
static bool allocation_fail;
static void *test_malloc(size_t n){return allocation_fail?NULL:malloc(n);}
static void *test_calloc(size_t n,size_t size){return allocation_fail?NULL:calloc(n,size);}
#define malloc test_malloc
#define calloc test_calloc
static mqtt_account_t mqtt_snapshot(void){return account;}
void feature_config_get(feature_config_t *out){*out=config;}
static void root_for(const feature_config_t*c,unsigned index,char*out){(void)index;mqtt_format(out,FEATURE_TOPIC_MAX,"phicomm/v2/%s/group/%s",account.key,c->group[0]?"group-id":"physical-id");}
typedef struct{char name[64];}a1_config_t;
static a1_config_t device;
static void a1_config_get(a1_config_t*out){*out=device;}
static unsigned language;
static unsigned device_language_get(void){return language;}
static const char *ph_i18n(unsigned l,const char*z,const char*e){return l?e:z;}
static int publish_error;
static bool mqtt_discovery_deferred;
static void discovery_resource_wait(void){assert(!"unexpected fixed HA writer failure");}
'''

class HaRuntimeContract(unittest.TestCase):
 def run_c(self,source):
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'probe.c').write_text(source)
   subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),'-I'+str(ROOT/'tests/host'),str(p/'probe.c'),'-o',str(p/'probe')],check=True)
   return subprocess.check_output([str(p/'probe')],text=True)

 def test_account_filters_bounds_rename_and_failure(self):
  runtime=(ROOT/'common/runtime.c').read_text()
  code='\n'.join(function(runtime,s) for s in ('unsigned feature_mqtt_filters(', 'bool feature_mqtt_filters_current('))
  for model in ('A1','M1'):
   with self.subTest(model=model):
    self.run_c(('#define FEATURE_DEVICE_A1 1\n' if model=='A1' else '')+PRELUDE+code+r'''
int main(void){
#ifdef FEATURE_DEVICE_A1
 const char*model="a1";unsigned cap=FEATURE_MQTT_A1_FILTER_MAX;
#else
 const char*model="m1";unsigned cap=FEATURE_MQTT_M1_FILTER_MAX;
#endif
 char user[64];memset(user,'x',63);user[63]=0;assert(mqtt_account_init(&account,1,model,user,LOCAL_ID));
 struct{unsigned before;char topics[4][FEATURE_TOPIC_MAX];unsigned after;} f;
 f.before=0x12345678;f.after=0x87654321;
 for(unsigned group=0;group<2;group++)for(unsigned peers=0;peers<=FEATURE_MAX_PEERS;peers++){
  memset(&config,0,sizeof(config));config.peer_count=peers;if(group)strcpy(config.group,"same-name");
#ifdef FEATURE_DEVICE_A1
  unsigned expected=2+(group||peers?1:0)+(peers?1:0);
#else
  unsigned expected=1;
#endif
  memset(f.topics,0x5a,sizeof(f.topics));char saved[sizeof(f.topics)];memcpy(saved,f.topics,sizeof(saved));
  assert(feature_mqtt_filters(f.topics,expected-1)==FEATURE_MQTT_FILTER_ERROR);assert(!memcmp(saved,f.topics,sizeof(saved)));
  assert(feature_mqtt_filters(f.topics,cap)==expected);for(unsigned i=0;i<expected;i++)assert(strstr(f.topics[i],account.key));
#ifdef FEATURE_DEVICE_A1
  if(peers)assert(strstr(f.topics[expected-1],"/sensor/+/+"));
#else
  assert(strstr(f.topics[0],"/sensor/aabbccddeeff/command"));
#endif
  assert(feature_mqtt_filters_current(f.topics,expected));config.revision++;assert(feature_mqtt_filters_current(f.topics,expected));
  allocation_fail=true;assert(feature_mqtt_filters(f.topics,cap)==FEATURE_MQTT_FILTER_ERROR);assert(!feature_mqtt_filters_current(f.topics,expected));allocation_fail=false;
  assert(!feature_mqtt_filters_current(f.topics,cap+1));assert(f.before==0x12345678&&f.after==0x87654321);
 }
 assert(mqtt_account_init(&account,1,model,"bob",LOCAL_ID));assert(!feature_mqtt_filters_current(f.topics,1));
 return 0;
}
''')

 def test_virtual_names_stable_uid_and_all_optional_tombstones(self):
  runtime=(ROOT/'common/runtime.c').read_text()
  layout=runtime[runtime.index('typedef struct {\n  char group[33];'):runtime.index('static discovery_layout_t *published_layout;')]
  code='\n'.join(function(runtime,s) for s in ('static void discovery_layout_capture(', 'static bool virtual_identity(', 'static const char *virtual_device_name(', 'bool feature_ha_device(', 'static bool discovery_virtual_step('))
  output=self.run_c('#define FEATURE_DEVICE_A1 1\n'+PRELUDE+layout+code+r'''
static unsigned scenario;
static int sender(const char*t,const void*b,size_t n,bool retained){assert(retained);printf("%u\t%s\t%.*s\n",scenario,t,(int)n,(const char*)b);return 0;}
int main(void){
 assert(mqtt_account_init(&account,1,"a1","alice",LOCAL_ID));
 const char*names[]={"Living Room A1","Bedroom A1",""};
 for(unsigned peers=1;peers<=FEATURE_MAX_PEERS;peers++)for(unsigned group=0;group<2;group++)for(unsigned name=0;name<3;name++)for(unsigned remove=0;remove<2;remove++){
  memset(&config,0,sizeof(config));config.peer_count=peers;strcpy(device.name,names[name]);if(group)strcpy(config.group,"客厅净化组");
  for(unsigned i=0;i<peers;i++)snprintf(config.peers[i].id,13,"%012x",i+1);
  char root[FEATURE_TOPIC_MAX];root_for(&config,0,root);discovery_layout_t layout;discovery_layout_capture(&layout,&config);
  char id[FEATURE_TOPIC_MAX],label[FEATURE_TOPIC_MAX];bool grouped;assert(feature_ha_device(id,sizeof(id),label,sizeof(label),&grouped));assert(grouped==(bool)group);assert(!strcmp(label,group?config.group:device.name[0]?device.name:"Phicomm A1"));
  printf("META\t%u\t%s\t%s\n",scenario,id,label);
  unsigned cursor=0;for(unsigned turn=0;turn<60;turn++)if(discovery_virtual_step(sender,&layout,root,remove,&cursor))break;
  assert(cursor==(peers>1?peers+1:1)*11);assert(!publish_error);scenario++;
 }
 allocation_fail=true;char id[512],name[512];bool grouped;assert(!feature_ha_device(id,sizeof(id),name,sizeof(name),&grouped));
 return 0;
}
''')
  scenarios={}
  for line in output.splitlines():
   if line.startswith('META\t'):
    _,n,parent,label=line.split('\t');scenarios[int(n)]={'parent':parent,'label':label,'packets':[]}
   else:
    n,topic,raw=line.split('\t');scenarios[int(n)]['packets'].append((topic,json.loads(raw) if raw else None))
  self.assertEqual(len(scenarios),48)
  for n,data in scenarios.items():
   peers=n//12+1;group=(n//6)%2;remove=n%2;sets=peers+1 if peers>1 else 1
   packets=data['packets'];self.assertEqual(len(packets),4+7*sets)
   self.assertEqual(sum(body is None for _,body in packets),len(packets) if remove else 7*sets)
   if not remove:
    for i,(topic,body) in enumerate(packets[:4]):
     self.assertEqual(body['device']['name'],data['label']);self.assertEqual(body['device']['identifiers'],[data['parent']])
     self.assertEqual(body['device']['model'],'A1 Air Purifier (EMW3080B)')
     self.assertEqual(body['name'],['空气净化器','灯环','童锁','灯环常亮'][i])
     self.assertEqual(body['unique_id'],topic.split('/')[-3]+'_'+topic.split('/')[-2]);self.assertTrue(topic.startswith('homeassistant/'))
    self.assertEqual(packets[0][1]['icon'],'mdi:air-purifier')
   # Renaming changes labels only; entity topics and parent identity stay fixed.
   if n%6>=2:
    baseline=scenarios[n-(n%6)+n%2]
    self.assertEqual(data['parent'],baseline['parent']);self.assertEqual([t for t,_ in packets],[t for t,_ in baseline['packets']])

 def test_real_model_discovery_preserves_baseline_entity_ids(self):
  # 6950338 mqtt_identity/mqtt_ha naming contract is intentionally frozen here:
  # account-key + model + lowercase MAC + the historical entity key.
  for model in ('a1','m1'):
   with self.subTest(model=model):
    source=(ROOT/model.upper()/'mqtt.c').read_text()
    if model=='a1':
     code=source[source.index('static void topic(ha_json'):source.index('static void primary_snapshot(')]
     code+=source[source.index('static int discovery_build(const a1_config_t *c'):source.index('static int ha_state(')]
     count=12
    else:
     code=source[source.index('enum { MQTT_BRIGHTNESS_INDEX'):source.index('static int ha_particles(')]
     count=15
    prelude=r'''
#include "common/ha_json.h"
#include "common/mqtt_account.h"
#include <assert.h>
#include <stdio.h>
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 1
#define PH_MQTT_ERROR_APP_DISCOVERY_IDENTITY 1
#define FEATURE_TOPIC_MAX MQTT_TOPIC_MAX
#define MQTT_FAILURE -1
#define A1_VERSION "rc4"
#define M1_VERSION "rc4"
#define A1_FILTER_MAX_HOURS 20000
#define A1_FRESH_AIR
#define M1_BAROMETER_STALE_MS 20000
static mqtt_account_t account;
static char base[192],availability_topic[512],command_topic[512];
typedef struct{char name[64];}a1_config_t;
typedef struct{char name[64];}m1_config_t;
static int m1_lock;
static struct{struct{bool received;}scd40,barometer;bool mcu_sensor_details;}m1_state={{true},{true},true};
static void mico_rtos_lock_mutex(int*p){(void)p;}
static void mico_rtos_unlock_mutex(int*p){(void)p;}
static bool feature_bound(void){return false;}
static bool feature_ha_device(char*i,size_t n,char*l,size_t m,bool*g){(void)i;(void)n;(void)l;(void)m;(void)g;return false;}
static int ph_mqtt_app_error(int a,int b){(void)a;return b;}
static int device_language_get(void){return 1;}
static const char*ph_i18n(int l,const char*z,const char*e){return l?e:z;}
static int publish(const char*t,const char*b,bool r){assert(r);printf("%s\t%s\n",t,b);return 0;}
'''
    main=r'''
int main(void){
 const char*users[]={"","alice"};
 for(unsigned u=0;u<2;u++){
 assert(mqtt_account_init(&account,1,"MODEL",users[u],"AABBCCDDEEFF"));strcpy(base,account.base);mqtt_leaf(&account,"availability",availability_topic,sizeof(availability_topic));mqtt_leaf(&account,"ha/command",command_topic,sizeof(command_topic));
 MODEL_config_t c={"Same physical device"};
 for(unsigned i=0;i<COUNT;i++){char bytes[3072];ha_json w;ha_json_init(&w,bytes,sizeof(bytes));assert(!discovery_build(&c,i,&w));}
 }
 return 0;
}
'''.replace('MODEL',model).replace('COUNT',str(count))
    rows=self.run_c(prelude+code+main).splitlines();self.assertEqual(len(rows),count*2)
    for index,row in enumerate(rows):
     topic,raw=row.split('\t');body=json.loads(raw)
     key='local' if index<count else 'u_616c696365'
     entity=topic.split('/')[-2]
     self.assertEqual(body['unique_id'],key+'_'+entity)
     self.assertEqual(body['device']['identifiers'],[key+'_'+model+'_aabbccddeeff'])
     self.assertTrue(topic.startswith('homeassistant/'));self.assertEqual(topic.split('/')[-3],key)
     self.assertTrue(entity.startswith(model+'_aabbccddeeff_'))
     if model=='m1' and index%count in (0,1):
      self.assertIn('unit_of_measurement',body) # temperature/humidity remain normal sensors.

 def test_tc1_legacy_energy_ids_and_names_are_preserved(self):
  output=self.run_c(r'''
#include "common/ha_json.h"
#include "common/mqtt_account.h"
#include <assert.h>
#include <stdlib.h>
#define MAX_MQTT_DATA_SIZE 2048
#define PH_MQTT_MORE 1
#define VERSION "rc4"
static mqtt_account_t account;
static bool synchronous_send;
static int synchronous_error;
static struct{struct{unsigned language;}bemfa;char socket_names[6][64];}config,*user_config=&config;
static struct{struct{char name[64];}micoSystemConfig;}system_data,*sys_config=&system_data;
static const char*ph_i18n(unsigned l,const char*z,const char*e){return l?e:z;}
static int UserMqttSendTopic(char*t,char*b,char retained){assert(retained);printf("%u\t%s\t%s\n",account.mode,t,b);return 0;}
#include "TC1/mqtt_server/ha_discovery.inc"
int main(void){
 strcpy(system_data.micoSystemConfig.name,"TC1 renamed device");for(unsigned i=0;i<6;i++)snprintf(config.socket_names[i],64,"插座 %u",i+1);
 for(unsigned mode=0;mode<2;mode++)for(unsigned l=0;l<2;l++){
 assert(mqtt_account_init(&account,mode,"tc1","alice","aabbccddeeff"));config.bemfa.language=l;
 for(unsigned i=0;i<17;i++)tc1_ha_discovery(i);
 }
 return 0;
}
''')
  entities=['socket_'+str(i) for i in range(6)]+['total_socket','led','childLock','reboot','soft_reboot','power','powerConsumption','startupTime','powerConsumptionToday','powerConsumptionYesterday','energySession']
  old=['s'+str(i) for i in range(6)]+['total_socket','led','child_lock','reboot','soft_reboot','p','pc','sut','pc_today','pc_yesterday','energy_session']
  rows=output.splitlines();self.assertEqual(len(rows),68)
  for n,row in enumerate(rows):
   mode,topic,raw=row.split('\t');body=json.loads(raw);i=n%17
   identity='u_616c696365_tc1_aabbccddeeff' if int(mode) else 'tc1_AABBCCDDEEFF'
   expected=identity+'_'+(entities[i] if int(mode) else old[i])
   self.assertEqual(body['uniq_id'],expected);self.assertEqual(body['object_id'],expected)
   self.assertEqual(body['device']['identifiers'],[identity]);self.assertEqual(body['device']['name'],'TC1 renamed device')
   if i<6:self.assertEqual(body['name'],'插座 '+str(i+1))
   if i>=11:
    self.assertEqual(body['value_template'],'{{ value_json.'+entities[i]+' }}')
    self.assertTrue(body['state_topic'].endswith('/ha/sensor'))
    if i!=13:self.assertEqual(body['unit_of_measurement'],'W' if i==11 else 'kWh')
   self.assertTrue(body['json_attributes_topic'].endswith('/ha/state'))

if __name__=='__main__':unittest.main()
