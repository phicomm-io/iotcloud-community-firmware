"""Compile the production discovery builder; inspect every additional entity."""
import unittest
from tests.test_stock_runtime import ROOT, run_host
from tests.test_http_json_budget import function

class ParticlesDiscovery(unittest.TestCase):
 def test_production_discovery(self):
  source=(ROOT/'M1/mqtt.c').read_text()
  enum=source[source.index('enum { MQTT_BRIGHTNESS_INDEX'):source.index('static int discovery_build')]
  harness=r'''
#include "M1/m1.h"
#include "common/mqtt/service.h"
#define MQTT_SUCCESS 0
static mqtt_account_t account;
static char base[192],availability_topic[512],command_topic[512];
m1_state_t m1_state;mico_mutex_t m1_lock;
static unsigned cleared,sent;static json_object *last;
static int publish(const char*t,const char*p,bool r){assert(t&&p&&r);assert(!*p);cleared++;return 0;}
void service_mqtt_json_end(service_mqtt_json_scope*s){(void)s;}
int service_mqtt_json_error(service_mqtt_json_scope*s,unsigned code){(void)s;(void)code;return -1;}
int service_mqtt_optional_send(int(*sender)(const char*,const char*,bool),const char*t,json_object*o,bool retain,service_mqtt_json_scope*s){
 (void)sender;(void)s;assert(t&&retain);sent++;if(last)json_object_put(last);last=o;return 0;
}
PRODUCTION
int main(void){
 assert(mqtt_account_init(&account,1,"m1","particles","aabbccddeeff"));strcpy(base,account.base);
 mqtt_leaf(&account,"availability",availability_topic,sizeof(availability_topic));
 m1_config_t c={0};strcpy(c.name,"test");service_mqtt_json_scope scope={0};
 for(unsigned i=MQTT_PARTICLES_INDEX;i<MQTT_DISCOVERY_COUNT;i++)assert(!discovery_build(&c,i,&scope));
 assert(cleared==8 && !sent);m1_state.mcu_sensor_details=true;
 for(unsigned i=MQTT_PARTICLES_INDEX;i<MQTT_DISCOVERY_COUNT;i++){
  assert(!discovery_build(&c,i,&scope));assert(last);
  const char *topic=json_object_get_string(json_object_object_get(last,"state_topic"));assert(strstr(topic,"/particles"));
  assert(json_object_get_int(json_object_object_get(last,"expire_after"))==6);
  assert(!strcmp(json_object_get_string(json_object_object_get(last,"availability_mode")),"all"));
  json_object *availability=json_object_object_get(last,"availability");assert(json_object_array_length(availability)==2);
  const char *tmpl=json_object_get_string(json_object_object_get(last,"value_template"));
  assert(strstr(tmpl,"if value_json.available else none"));
  assert(strstr(tmpl,i<MQTT_PARTICLES_INDEX+2?"atmospheric_ug_m3":"particles_per_0_1_l"));
  if(i>=MQTT_PARTICLES_INDEX+2)assert(!json_object_object_get(last,"device_class"));
 }
 assert(sent==8);json_object_put(last);puts("M1 production particle discovery: 8 entities, independent topic, availability, expiry and legacy cleanup OK");
}
'''
  production=function(source,'static void str(')+enum+function(source,'static int discovery_build(')
  run_host(harness.replace('PRODUCTION',production))
