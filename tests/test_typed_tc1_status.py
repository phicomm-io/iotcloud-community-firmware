"""Actual TC1 capture with pinned SDK layouts and direct native MP output.

Normal schema is checked by test_status_views; this test owns fixed SDK source
bounds, guard admission, post-unlock getters, secret exclusion and freezing.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from tests.test_runtime_mqtt_mp import function
ROOT=Path(__file__).resolve().parents[1]

class TypedTC1Status(unittest.TestCase):
 def test_real_sdk_capture_and_source_boundaries(self):
  capture=function((ROOT/'TC1/main.c').read_text(),'bool tc1_status_capture(')
  fixture=r'''
#include "TC1/main.h"
#include "TC1/user_wifi.h"
#include "TC1/mqtt_server/user_mqtt_client.h"
#include "common/device_status.h"
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
static ph_buf *encode(const device_status_t*s,const device_status_full_t*f){ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));device_status_mp(&w,s,f);ph_buf*b=phw_take(&w);assert(b);return b;}
static void text(const ph_value*root,const char*key,const char*expected){ph_value v;assert(phv_get(root,key,&v)&&phv_equal_text(&v,expected));}
static bool contains(const ph_buf*b,const char*s){for(size_t i=0;i+strlen(s)<=b->len;i++)if(!memcmp(b->buf+i,s,strlen(s)))return true;return false;}
int main(void){
 assert(sizeof(sys_config->micoSystemConfig.name)==32);assert(sizeof(sys_config->micoSystemConfig.ssid)==32);
 memset(sys_config,0,sizeof(*sys_config));memset(&user,0,sizeof(user));
 memset(sys_config->micoSystemConfig.name,'N',32);memset(sys_config->micoSystemConfig.ssid,'S',32);
 strcpy(sys_config->micoSystemConfig.user_key,"SECRET_SENTINEL_MUST_NOT_ESCAPE");strcpy(user.mqtt_password,"MQTT_SECRET");strcpy(user.web_password,"WEB_SECRET");
 for(unsigned i=0;i<6;i++){memset(user.socket_names[i],'A'+i,sizeof(user.socket_names[i]));user.socket_status[i]=i%2;}
 memory.free_memory=30000;device_status_t snapshot;device_status_full_t full;
 busy=true;assert(!tc1_status_capture(&snapshot,NULL));assert(!tc1_status_capture(&snapshot,&full));assert(!expensive&&!held);
 busy=false;assert(tc1_status_capture(&snapshot,NULL));assert(!expensive&&!held);
 assert(strlen(snapshot.identity.name)==32&&!memcmp(snapshot.identity.name,sys_config->micoSystemConfig.name,32));
 ph_value root,v;ph_writer unavailable;assert(phw_init(&unavailable,1));device_status_mp(&unavailable,&snapshot,NULL);assert(!phw_take(&unavailable)); /* public contract is full only */
 assert(tc1_status_capture(&snapshot,&full));assert(expensive==2&&!held);
 assert(strlen(full.network.ssid)==32&&!memcmp(full.network.ssid,sys_config->micoSystemConfig.ssid,32));
 for(unsigned i=0;i<6;i++)assert(strlen(full.model.tc1.socket_names[i])==63);
 allowed=false;ph_buf*first=encode(&snapshot,&full);assert(phv_parse(1,first->buf,first->len,&root));
 assert(!contains(first,"SECRET")&&!contains(first,"MQTT_SECRET")&&!contains(first,"WEB_SECRET"));
 char expected[33];memset(expected,'S',32);expected[32]=0;text(&root,"ssid",expected);
 assert(phv_get(&root,"socket_names",&v)&&v.kind==PHV_ARRAY&&v.count==6);
 for(unsigned i=0;i<6;i++){char label[64];memset(label,'A'+i,63);label[63]=0;ph_value item;assert(phv_at(&v,i,&item)&&phv_equal_text(&item,label));}
 text(&root,"model","tc1");text(&root,"device_id","aabbccddeeff");text(&root,"wifi_trial_status","idle");text(&root,"ota_state","idle");
 assert(phv_get(&root,"socket_states",&v)&&v.count==6);for(unsigned i=0;i<6;i++){ph_value item;int64_t state;assert(phv_at(&v,i,&item)&&phv_int(&item,&state)&&state==(int64_t)(i%2));}
 const char*flags[]={"wifi_password_set","mqtt_password_set","web_password_set","wifi_connected","mqtt_connected","child_lock"};
 for(unsigned i=0;i<sizeof(flags)/sizeof(*flags);i++){bool yes;assert(phv_get(&root,flags[i],&v)&&phv_bool(&v,&yes)&&yes);}
 const char*numbers[]={"utc_time","free_memory","uptime_seconds","ota_progress"};const int64_t values[]={1700000000,30000,25,0};
 for(unsigned i=0;i<4;i++){int64_t number;assert(phv_get(&root,numbers[i],&v)&&phv_int(&v,&number)&&number==values[i]);}
 memset(sys_config,0xa5,sizeof(*sys_config));memset(&user,0xa5,sizeof(user));
 ph_buf*second=encode(&snapshot,&full);assert(first->len==second->len&&!memcmp(first->buf,second->buf,first->len));ph_buf_free(first);ph_buf_free(second);
 puts("TC1 SDK name/SSID32, outlet63, guarded capture, secret-free frozen native MP PASS");return 0;
}
'''.replace('CAPTURE',capture)
  sdk=Path(os.environ.get('PHICOMM_TC1_SDK',ROOT/'build/tc1-sdk-3.6.4/mico-os'))
  cc=Path(os.environ.get('ARM_CC',ROOT/'build/tc1-tools/gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc'))
  qemu=shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
  self.assertTrue((sdk/'include/mico.h').is_file(),'prepare pinned TC1 SDK or set PHICOMM_TC1_SDK')
  self.assertTrue(cc.is_file(),'prepare pinned compiler or set ARM_CC');self.assertTrue(qemu,'qemu-arm required')
  includes=['include','platform','platform/include','platform/Cortex-M4','platform/Cortex-M4/CMSIS','platform/MCU/MW3xx','platform/MCU/MW3xx/peripherals','platform/MCU/MW3xx/interface','board/MK3031','MiCO/system','MiCO/system/include','libraries/utilities','MiCO/security','MiCO/security/TLS/mocSSL','libraries/drivers','libraries/drivers/MiCOKit_EXT']
  includes+=['platform/MCU/MW3xx/peripherals/sdk/src/incl/'+p for p in ('sdk','platform/os/micortos','sdk/drivers','sdk/drivers/mw300','sdk/drivers/mw300/regs')]
  flags=['-std=gnu11','-DMOC=1','-DMW310','-DCONFIG_VARIANT_MW310','-I'+str(ROOT),'-I'+str(ROOT/'TC1'),'-I'+str(ROOT/'common'),*['-I'+str(sdk/p) for p in includes]]
  files=[str(ROOT/p) for p in ('common/device_status.c','common/typed_wire.c','common/wire_buffer.c')]
  with tempfile.TemporaryDirectory() as tmp:
   d=Path(tmp);source=d/'probe.c';source.write_text(fixture)
   for host in (False,True):
    binary=d/('probe-host' if host else 'probe-arm')
    if host:command=[os.environ.get('CC','cc'),*flags,'-Dsize_t=size_t','-Dssize_t=ssize_t','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(source),*files,'-lm','-o',str(binary)]
    else:command=[str(cc),*flags,'-Os','-mthumb','-mcpu=cortex-m3','-nostartfiles','--specs=nano.specs','--specs=nosys.specs','-Wl,-e,_start,-Ttext=0x10000',str(source),*files,str(ROOT/'tests/http_core/json_arm_runtime.c'),'-lm','-o',str(binary)]
    subprocess.run(command,check=True,timeout=30);subprocess.run([str(binary)] if host else [qemu,str(binary)],check=True,timeout=30)
if __name__=='__main__':unittest.main()
