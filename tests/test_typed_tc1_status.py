"""Actual TC1 capture, pinned SDK object layout, common encoder under ARM ABI."""
from pathlib import Path
import unittest
from tests import test_http_json_budget as budget
ROOT=budget.ROOT
function=budget.function
class TypedTC1Status(unittest.TestCase):
 def test_real_sdk_capture_and_source_boundaries(self):
  capture=function((ROOT/'TC1/main.c').read_text(),'bool tc1_status_capture(')
  fixture=r'''
#include "TC1/main.h"
#include "TC1/user_wifi.h"
#include "TC1/mqtt_server/user_mqtt_client.h"
#include "common/device_status.h"
#include "common/json_build.h"
#include "json_heap.h"
#include <assert.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
static bool allowed=true,held,busy;
static unsigned expensive;
static system_config_t system_state;
system_config_t *sys_config=&system_state;
static user_config_t user;
user_config_t *user_config=&user;
char str_mac[16]="aabbccddeeff",rtc_init=1,wifi_status=WIFI_STATE_CONNECTED;
int childLockEnabled=1;
unsigned tc1_mqtt_status=MQTT_READY;
bool tc1_mqtt_cleanup_warning;
IpStatus ip_status;
static void getter(void){assert(allowed);}
bool tc1_status_begin(void){getter();assert(!held);if(busy)return false;held=true;return true;}
void tc1_status_unlock(void){assert(held);held=false;}
const char*WifiTrialStatus(void){getter();return "idle";}
void tc1_ota_snapshot(char*state,char*error,unsigned*progress){getter();assert(!held);strcpy(state,"idle");*error=0;*progress=0;}
uint32_t mico_rtos_get_time(void){getter();assert(!held);return 25000;}
static micoMemInfo_t memory;
micoMemInfo_t *MicoGetMemoryInfo(void){getter();assert(!held);expensive++;return &memory;}
time_t time(time_t*t){getter();assert(!held);expensive++;if(t)*t=1700000000;return 1700000000;}
CAPTURE
#include "common/device_status.c"

#include "TC1/http_server/json_buffer.h"
#include "common/firmware_variant.h"
OSStatus mico_time_get_time(mico_time_t*out){*out=mico_rtos_get_time();return kNoErr;}
char *GetSocketStatus(void){static char text[32];snprintf(text,sizeof(text),"%d,%d,%d,%d,%d,%d",user.socket_status[0],user.socket_status[1],user.socket_status[2],user.socket_status[3],user.socket_status[4],user.socket_status[5]);return text;}
json_object *service_parse(const void*bytes,size_t n){char*p=malloc(n+1);assert(p);memcpy(p,bytes,n);p[n]=0;json_object*o=json_tokener_parse(p);free(p);return o;}
#include "tests/http_core/legacy_tc1_status.c"
static void equal_json(json_object*a,json_object*b){
 assert((!a)==(!b));if(!a)return;assert(json_object_get_type(a)==json_object_get_type(b));
 if(json_object_is_type(a,json_type_object)){
  unsigned ac=0,bc=0;json_object_object_foreach(a,k,v){ac++;equal_json(v,json_object_object_get(b,k));}json_object_object_foreach(b,k2,v2){(void)k2;(void)v2;bc++;}assert(ac==bc);
 }else if(json_object_is_type(a,json_type_array)){
  assert(json_object_array_length(a)==json_object_array_length(b));for(unsigned i=0;i<json_object_array_length(a);i++)equal_json(json_object_array_get_idx(a,i),json_object_array_get_idx(b,i));
 }else assert(!strcmp(json_object_to_json_string(a),json_object_to_json_string(b)));
}

int main(void){
 assert(sizeof(sys_config->micoSystemConfig.name)==32);
 assert(sizeof(sys_config->micoSystemConfig.ssid)==32);
 memset(sys_config,0,sizeof(*sys_config));memset(&user,0,sizeof(user));
 memset(sys_config->micoSystemConfig.name,'N',32);memset(sys_config->micoSystemConfig.ssid,'S',32);
 strcpy(sys_config->micoSystemConfig.user_key,"SECRET_SENTINEL_MUST_NOT_ESCAPE");
 strcpy(user.mqtt_password,"MQTT_SECRET");strcpy(user.web_password,"WEB_SECRET");
 for(unsigned i=0;i<6;i++){memset(user.socket_names[i],'A'+i,sizeof(user.socket_names[i]));user.socket_status[i]=i%2;}
 memory.free_memory=30000;
 device_status_t snapshot;device_status_full_t full;
 /* Normal terminated source: compare every value/type/field to both real old
  * builders. Object insertion order is not part of the JSON wire schema. */
 strcpy(sys_config->micoSystemConfig.name,"normal name");strcpy(sys_config->micoSystemConfig.ssid,"normal ssid");
 for(unsigned i=0;i<6;i++)strcpy(user.socket_names[i],"outlet");
 assert(tc1_status_capture(&snapshot,&full));json_object*a=device_status_encode(&snapshot,&full),*b=legacy_tc1_status_json();equal_json(a,b);json_object_put(a);json_object_put(b);
 assert(tc1_status_capture(&snapshot,NULL));a=device_status_encode(&snapshot,NULL);b=legacy_tc1_status_live_json();equal_json(a,b);json_object_put(a);json_object_put(b);
 memset(sys_config->micoSystemConfig.name,'N',32);memset(sys_config->micoSystemConfig.ssid,'S',32);
 for(unsigned i=0;i<6;i++)memset(user.socket_names[i],'A'+i,sizeof(user.socket_names[i]));expensive=0;

 busy=true;assert(!tc1_status_capture(&snapshot,NULL));assert(!expensive&&!held);
 busy=false;assert(tc1_status_capture(&snapshot,NULL));assert(!expensive&&!held);
 assert(strlen(snapshot.identity.name)==32&&!memcmp(snapshot.identity.name,sys_config->micoSystemConfig.name,32));
 json_object*live=device_status_encode(&snapshot,NULL);assert(live&&!json_object_object_get(live,"ssid"));json_object_put(live);
 assert(tc1_status_capture(&snapshot,&full));assert(expensive==2&&!held);
 assert(strlen(full.network.ssid)==32&&!memcmp(full.network.ssid,sys_config->micoSystemConfig.ssid,32));
 for(unsigned i=0;i<6;i++)assert(strlen(full.model.tc1.socket_names[i])==63);
 allowed=false;
 json_object*o=device_status_encode(&snapshot,&full);assert(o);const char*wire=json_object_to_json_string(o);
 assert(!strstr(wire,"SECRET")&&!strstr(wire,"MQTT_SECRET")&&!strstr(wire,"WEB_SECRET"));
 assert(strlen(json_object_get_string(json_object_object_get(o,"ssid")))==32);
 char*saved=strdup(wire);json_object_put(o);
 memset(sys_config,0xa5,sizeof(*sys_config));memset(&user,0xa5,sizeof(user));
 o=device_status_encode(&snapshot,&full);assert(o&&!strcmp(saved,json_object_to_json_string(o)));free(saved);json_object_put(o);
 json_heap_stats_t stats;json_heap_snapshot(&stats);assert(!stats.live_blocks);
 printf("TC1 actual SDK name/SSID32 bounded + frozen pure full/live PASS base=%u full=%u\n",(unsigned)sizeof(snapshot),(unsigned)sizeof(full));
 return 0;
}
'''.replace('CAPTURE',capture)
  budget.JsonBudget().compile_arm('TC1',fixture,budget=False,product=False,host=True)
if __name__=='__main__':unittest.main()
