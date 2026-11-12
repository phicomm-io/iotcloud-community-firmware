"""Bounded metadata/state MQTT dispatch in both A1 and M1 consumers."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class SensorMPTest(unittest.TestCase):
 def test_consumers(self):
  source=r'''
#include <assert.h>
#include "M1/m1.h"
#include "common/sensor_mp.h"
static uint32_t now=1000;m1_state_t m1_state;mico_mutex_t m1_lock;
uint32_t mico_rtos_get_time(void){return now;}
static uint32_t now_ms(void){return now;}
static mqtt_account_t account;
bool feature_mac(const char *s,char out[13]){if(strlen(s)!=12)return false;for(unsigned i=0;i<12;i++){if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;out[i]=s[i];}out[12]=0;return true;}
#ifdef FEATURE_DEVICE_A1
static feature_config_t configuration;static feature_sample_t samples[FEATURE_MAX_PEERS];
static struct {bool lan;uint32_t last_seen,retired_boot[4];} peers[FEATURE_MAX_PEERS];
static bool locked;static void lock(void){assert(!locked);locked=true;}static void unlock(void){assert(locked);locked=false;}
#else
static m1_co2_source_config_t config={.external=true,.revision=1,.device_id="123456abcdef"};
void m1_co2_source_config_get(m1_co2_source_config_t *o){*o=config;}
const char *m1_co2_source_config_save(const m1_co2_source_config_t *c){config=*c;return "accepted";}
static unsigned metadata_calls,state_calls;
void m1_display_source_metadata(const char *id,const ph_value *v){assert(!strcmp(id,config.device_id)&&v->kind==PHV_MAP);metadata_calls++;}
void m1_display_source_sample(const char *id,const ph_value *v){assert(!strcmp(id,config.device_id)&&v->kind==PHV_MAP);state_calls++;}
#include "M1/co2_source.c"
#endif
#ifdef FEATURE_DEVICE_A1
#define SERVICE_CACHE_STATUS 1
static void service_cache_changed(unsigned mask){assert(mask==SERVICE_CACHE_STATUS);}
#endif
#include "common/runtime_sensor_mp.inc"
static bool valid(void){
#ifdef FEATURE_DEVICE_A1
return samples[0].co2_valid;
#else
uint16_t ppm,ttl;return m1_co2_source_value(&ppm,&ttl);
#endif
}
static unsigned reading(void){
#ifdef FEATURE_DEVICE_A1
return (unsigned)samples[0].co2;
#else
uint16_t ppm,ttl;assert(m1_co2_source_value(&ppm,&ttl));return ppm;
#endif
}
static void dispatch(ph_writer *w,bool metadata,bool retained){ph_buf *b=phw_take(w);assert(b);char topic[FEATURE_TOPIC_MAX];assert(mqtt_aux(&account,"sensor",metadata?"123456abcdef/metadata":"123456abcdef/state",topic,sizeof(topic)));assert(runtime_sensor_input_mp(&account,topic,b->buf,b->len,retained));memset(b->buf,0,b->len);ph_buf_free(b);}
static void meta(unsigned boot,unsigned revision,const char *unit){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));ph_sensor_metadata_begin(&w,"123456abcdef",boot,revision);ph_sensor_descriptor_write(&w,"co2","CO2",unit);ph_sensor_descriptor_write(&w,"temperature","Temperature","°C");ph_sensor_finish(&w);dispatch(&w,true,true);}
static void state(unsigned boot,unsigned seq,unsigned revision,bool present,bool good,unsigned value,bool retained){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));ph_sensor_snapshot_begin(&w,"123456abcdef",boot,seq,revision);if(present)ph_sensor_reading_write(&w,"co2",value,good,0);ph_sensor_finish(&w);dispatch(&w,false,retained);}
int main(void){
assert(mqtt_account_init(&account,MQTT_TOPIC_TMCP,"m1","owner","aabbccddeeff"));
#ifdef FEATURE_DEVICE_A1
configuration.peer_count=1;strcpy(configuration.peers[0].id,"123456abcdef");
#endif
sensor_descriptors_reset();state(1,1,1,true,true,500,false);assert(!valid());meta(1,1,"ppm");state(1,2,1,true,true,600,false);assert(valid()&&reading()==600);
meta(1,1,"ppm");assert(valid());state(1,2,1,true,true,900,false);assert(reading()==600);state(1,3,1,true,true,700,true);assert(reading()==600);
state(1,3,1,false,false,0,false);assert(!valid());state(1,4,1,true,true,800,false);assert(reading()==800);meta(1,2,"bad");assert(!valid());state(1,5,2,true,true,800,false);assert(!valid());
meta(2,1,"ppm");state(2,1,1,true,true,900,false);assert(reading()==900);meta(1,99,"ppm");state(1,99,99,true,true,1000,false);assert(valid()&&reading()==900);state(2,2,1,true,true,901,false);assert(reading()==901);
#ifdef FEATURE_DEVICE_A1
peers[0].lan=true;peers[0].last_seen=now;state(2,3,1,true,true,2000,false);assert(reading()==901);now+=FEATURE_STALE_MS;state(2,4,1,true,true,1000,false);assert(reading()==1000);assert(!locked);
#else
now+=19999;assert(valid());now++;assert(!valid());assert(metadata_calls==5&&state_calls==9);
#endif
/* Namespace ownership is enforced before decoding even valid sensor bytes. */
ph_writer boundary;assert(phw_init(&boundary,PH_WIRE_MSGPACK));ph_sensor_snapshot_begin(&boundary,"123456abcdef",2,99,1);ph_sensor_finish(&boundary);
ph_buf *payload=phw_take(&boundary);assert(payload);mqtt_account_t outsider;char outside[FEATURE_TOPIC_MAX];
assert(mqtt_account_init(&outsider,MQTT_TOPIC_TMCP,"m1","other","aabbccddeeff"));
assert(mqtt_aux(&outsider,"sensor","123456abcdef/state",outside,sizeof(outside)));
assert(!runtime_sensor_input_mp(&account,outside,payload->buf,payload->len,false));ph_buf_free(payload);
/* Cache reset requires metadata again; an old snapshot cannot renew its lease. */
sensor_descriptors_reset();state(2,100,1,true,true,1100,false);assert(!valid());
meta(3,1,"ppm");state(3,1,1,true,true,1100,false);assert(reading()==1100);
state(3,2,2,true,true,1200,false);assert(!valid());
meta(3,2,"ppm");state(3,3,2,true,true,1200,false);assert(reading()==1200);
/* Reject malformed counter kinds/ranges before forwarding to either lane. */
for(unsigned key=0;key<3;key++)for(unsigned kind=0;kind<6;kind++){
 const char *keys[]={"boot","sequence","descriptor_revision"};
 assert(phw_init(&boundary,PH_WIRE_MSGPACK));phw_begin(&boundary,NULL,false);
 phw_int(&boundary,"schema",3);phw_text(&boundary,"device_id","123456abcdef");
 for(unsigned i=0;i<3;i++){
  if(i!=key)phw_u32(&boundary,keys[i],i==1?99:3);
  else switch(kind){case 0:phw_int(&boundary,keys[i],0);break;case 1:phw_int(&boundary,keys[i],-1);break;
   case 2:phw_int(&boundary,keys[i],INT64_C(4294967296));break;case 3:phw_number(&boundary,keys[i],1.5);break;
   case 4:phw_bool(&boundary,keys[i],true);break;default:phw_text(&boundary,keys[i],"1");break;}
 }
 phw_begin(&boundary,"sensors",false);phw_end(&boundary);phw_end(&boundary);dispatch(&boundary,false,false);
 assert(reading()==1200);
}
return 0;}
'''
  with tempfile.TemporaryDirectory() as d:
   src=Path(d)/'test.c';src.write_text(source)
   json_dir=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',ROOT/'build/m1-sdk/mico-os/libraries/utilities/json_c'))
   for model in ('A1','M1'):
    exe=Path(d)/model
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT/'tests/host'),'-I',str(ROOT),'-I',str(json_dir),*(['-DFEATURE_DEVICE_A1'] if model=='A1' else []),str(src),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
