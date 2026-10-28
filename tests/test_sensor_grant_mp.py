"""Actual native MQTT-grant receiver: binary token, boot, expiry and capacity."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class SensorGrant(unittest.TestCase):
 def test_native_receiver(self):
  source=(ROOT/'common/runtime.c').read_text()
  start=source.index('bool feature_sensor_session_mp(')
  body=source[start:source.index('\n#endif',start)]
  harness=r'''
#include <assert.h>
#include <string.h>
#include "common/lan_wire.inc"
#define FEATURE_MAX_PEERS 4
#define SENSOR_SESSION_IDLE_MS 1800000u
#define LOCAL_ID "001122334455"
typedef struct {char id[13],ip[16];uint8_t token[16];uint32_t boot,last_seen,last_read;bool valid,active;} sensor_session_t;
static sensor_session_t sensor_sessions[4];static uint32_t ticks=1;
static uint32_t now_ms(void){return ticks;}static void lock(void){}static void unlock(void){}
BODY
static bool grant(const char *peer,uint32_t boot,unsigned version,size_t token_size,const char **code){
 unsigned char storage[256],token[16];memset(token,0xa1,sizeof(token));ph_buf buf;ph_writer w;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buf,storage,sizeof(storage)));phw_begin(&w,NULL,false);phw_uint(&w,"v",version);phw_text(&w,"a1",peer);phw_u32(&w,"boot",boot);phw_bytes_value(&w,"token",token,token_size);phw_end(&w);size_t n=phw_finish_fixed(&w);ph_value params;assert(phv_parse(PH_WIRE_MSGPACK,storage,n,&params));bool ok=feature_sensor_session_mp(&params,code);memset(storage,0,sizeof(storage));return ok;
}
int main(void){const char *code;
 assert(grant("aabbccddeeff",UINT32_MAX,3,16,&code));assert(sensor_sessions[0].boot==UINT32_MAX&&sensor_sessions[0].token[0]==0xa1&&!strcmp(code,"accepted"));
 assert(!grant("aabbccddeeff",1,2,16,&code)&&!strcmp(code,"invalid_params"));assert(!grant("aabbccddeeff",1,3,15,&code));assert(!grant("aabbccddeeff",0,3,16,&code));assert(!grant(LOCAL_ID,1,3,16,&code));
 assert(grant("000000000001",1,3,16,&code));assert(grant("000000000002",1,3,16,&code));assert(grant("000000000003",1,3,16,&code));assert(!grant("000000000004",1,3,16,&code)&&!strcmp(code,"busy"));
 ticks+=SENSOR_SESSION_IDLE_MS;assert(grant("000000000004",1,3,16,&code));assert(!strcmp(sensor_sessions[0].id,"000000000004"));return 0;
}
'''.replace('BODY',body)
  with tempfile.TemporaryDirectory() as temp:
   probe=Path(temp)/'probe.c';probe.write_text(harness);exe=Path(temp)/'probe'
   subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-error=misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),str(probe),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
