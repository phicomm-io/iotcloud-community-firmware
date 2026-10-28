"""Native MessagePack transport borrowing, NUL and retry coverage."""
from pathlib import Path
import tempfile
import unittest
from tests import test_shared_mqtt as shared
ROOT=shared.ROOT

class NativeMqtt(unittest.TestCase):
    def test_native_discovery_schema_and_fixed_bounds(self):
        import subprocess
        source = r'''
#include "common/mqtt/discovery_mp.h"
#include <assert.h>
int main(void){
 mqtt_account_t account;unsigned char bytes[512];
 const char *models[]={"a1","m1","tc1"};
 for(unsigned i=0;i<3;i++){
  assert(mqtt_account_init(&account,1,models[i],"alice","aabbccddeeff"));
  size_t n=ph_mqtt_discovery_mp(&account,"1.0.0-rc.4","full",bytes,sizeof(bytes));assert(n);
  ph_value root,v;int64_t x;assert(phv_parse(1,bytes,n,&root));
  assert(phv_get(&root,"schema",&v)&&phv_int(&v,&x)&&x==2);
  assert(phv_get(&root,"model",&v)&&phv_equal_text(&v,models[i]));
  assert(phv_get(&root,"protocol",&v)&&phv_equal_text(&v,"tmcp"));
  assert(phv_get(&root,"firmware_version",&v)&&phv_equal_text(&v,"1.0.0-rc.4"));
  assert(phv_get(&root,"device_id",&v)&&phv_equal_text(&v,"aabbccddeeff"));
  assert(phv_get(&root,"wire_formats",&v)&&v.kind==PHV_ARRAY&&v.count==1);
  for(size_t cap=0;cap<=n;cap++)assert(!ph_mqtt_discovery_mp(&account,"1.0.0-rc.4","full",bytes,cap));
  assert(ph_mqtt_discovery_mp(&account,"1.0.0-rc.4","full",bytes,n+1)==n);
 }
 assert(!ph_mqtt_discovery_mp(&account,NULL,"full",bytes,sizeof(bytes)));return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            p=Path(td);(p/'test.c').write_text(source)
            subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-I'+str(ROOT),str(p/'test.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

    def test_actual_paho_receipt_embedded_nul_retry_and_release(self):
        fixture = r'''
#define OVERLOAD_FIXTURE 1
#define main old_paho_main
#include "tests/mqtt_core/paho.c"
#undef main
static unsigned binary_seen,released;static char original_target[MQTT_TOPIC_MAX];
static void release_binary(void *p){released++;free(p);}
static void overload_handler(MessageData *d){
 assert(d->message->payloadlen==2 && !memcmp(d->message->payload,"{}",2));
 unsigned char *wire=malloc(4);assert(wire);memcpy(wire,"\x81\xa1v\0",4);
 assert(ph_mqtt_input_consumed());heap_capacity=charged_live+100;
 mqtt_target_t target;mqtt_target_set(&target,&account);
 int rc=ph_mqtt_receipt_send_bytes(&target,wire,4,wire,release_binary);
 assert(rc==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);assert(!released);
}
static void overload_publication(const unsigned char *topic,unsigned topic_n,const unsigned char *body,size_t n){
 assert(topic_n==strlen(original_target)&&!memcmp(topic,original_target,topic_n));assert(n==4&&!memcmp(body,"\x81\xa1v\0",4));binary_seen++;
}
int main(void){
 assert(mqtt_account_init(&account,1,"tc1","test","aabbccddeeff"));assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
 assert(mqtt_leaf(&account,"result",original_target,sizeof(original_target)));
 init();incoming("rx/command","{}",99);assert(ph_mqtt_yield(&client,1)==0);
 assert(ph_mqtt_poll(&client)==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED);assert(!binary_seen&&!released);
 assert(mqtt_account_init(&account,1,"tc1","renamed","aabbccddeeff"));
 heap_capacity=40000;now_ms+=1000;assert(ph_mqtt_poll(&client)==0);assert(binary_seen==1&&released==1);
 cleanup();ph_mqtt_owner_end();assert(!native_live&&!charged_live);return 0;
}
'''
        harness=shared.SharedMQTT()
        with tempfile.TemporaryDirectory() as tmp, harness.protocol('TC1') as protocol:
            path=Path(tmp)/'probe.c';path.write_text(fixture)
            harness.compile_run([path,ROOT/'common/mqtt/core.c',ROOT/'common/mqtt/client.c',ROOT/'common/mqtt/session.c',ROOT/'common/typed_wire.c',ROOT/'common/wire_buffer.c',
                                 protocol/'MQTTClient.c',*sorted((protocol/'MQTTPacket').glob('*.c'))],
                                [ROOT,ROOT/'tests/mqtt_core/stubs',protocol,protocol/'mico',protocol/'MQTTPacket'],
                                ['-DPHICOMM_BINARY_MQTT=1','-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])

    def test_service_admission_and_borrowed_input(self):
        from tests.test_tc1_mqtt_stack import extract
        source = (ROOT / 'common/mqtt/service.c').read_text()
        code = extract(source, 'static void receipt_release(') + '\n' + extract(source, 'int service_mqtt_input(')
        fixture = r'''
#include <assert.h>
#include <stdlib.h>
#include "common/typed_wire.h"
#include "common/mqtt/core.h"
#include "common/mqtt_account.h"
#include "common/mqtt/binary_busy.h"
#include "common/typed_wire.c"
#include "common/wire_buffer.c"
#define MQTT_SUCCESS 0
#define MQTT_FAILURE -1
typedef int(*service_mqtt_sender)(const char*,const void*,size_t,bool);
static ph_mqtt_core core={.dispatch_depth=1};
static bool admit=true,active;
static unsigned calls,consumes;
static ph_buf *input;
static bool ph_mqtt_owner(void){return true;}
static const ph_mqtt_core *ph_mqtt_debug_core(void){return &core;}
static bool ota_job_active(void){return active;}
static bool ph_mqtt_input_admit(void){return admit;}
static bool ph_mqtt_input_consumed(void){memset(input->buf,0,input->len);consumes++;return true;}
ph_buf *service_mqtt_command_mp(const void *data,size_t n){
 ph_value root,v;assert(phv_parse(1,data,n,&root));assert(phv_get(&root,"request_id",&v)&&phv_equal_text(&v,"borrow-1"));calls++;
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"request_id","borrow-1");phw_text(&w,"code","accepted");phw_int(&w,"zero",0);phw_end(&w);return phw_take(&w);
}
static int ph_mqtt_receipt_send_bytes(const mqtt_target_t *target,const void *data,size_t n,void *owned,void(*release)(void *)){
 assert(target);ph_value root,id,code;assert(phv_parse(1,data,n,&root));assert(phv_get(&root,"request_id",&id)&&phv_equal_text(&id,"borrow-1"));
 assert(phv_get(&root,"code",&code)&&phv_equal_text(&code,admit?"accepted":"busy"));
 if(owned)release(owned);return 0;
}
'''+code+r'''
int main(void){
 mqtt_account_t account;assert(mqtt_account_init(&account,1,"tc1","test","aabbccddeeff"));
 for(unsigned i=0;i<2;i++){
  ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"request_id","borrow-1");phw_text(&w,"action","outlet.set");phw_begin(&w,"params",false);phw_bool(&w,"on",true);phw_end(&w);phw_end(&w);input=phw_take(&w);assert(input);
  admit=i==0;assert(!service_mqtt_input(NULL,&account,input->buf,input->len,false));assert(calls==1);ph_buf_free(input);
 }
 assert(consumes==2);return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path/'probe.c').write_text(fixture)
            import subprocess
            subprocess.run(['cc','-std=gnu99','-g','-fsanitize=address,undefined','-no-pie','-I'+str(ROOT),str(path/'probe.c'),'-o',str(path/'probe')],check=True)
            subprocess.run([str(path/'probe')],check=True)

    def test_tc1_business(self):
        import subprocess
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'test'
            subprocess.run(['cc','-std=gnu99','-g','-fsanitize=address,undefined','-no-pie','-I'+str(ROOT),str(ROOT/'tests/tc1_business_mp.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_tc1_exact_topic_binary_ingress(self):
        from tests.test_tc1_mqtt_stack import extract
        from tests import test_sensor_producers_mp as producers
        source=(ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text()
        body=extract(source,'static bool tc1_topic_bytes(')+'\n'+extract(source,'static bool ingress_accept(')
        producers.SensorProducers().compile_run(r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#define MQTT_TOPIC_MAX 64
#define MAX_MQTT_DATA_SIZE 64
static char command_topic[]="scope/device/command";
typedef struct{char *cstring;struct{int len;char *data;}lenstring;}MQTTString;
typedef struct{void *payload;size_t payloadlen;bool retained;}MQTTMessage;
typedef struct{MQTTString *topicName;MQTTMessage *message;}MessageData;
typedef void(*messageHandler)(MessageData *);
static void ha_birth(MessageData *d){(void)d;}
static void command(MessageData *d){(void)d;}
''' + body + r'''
int main(void){
 unsigned char binary[]={0x81,0xa1,'v',0};char borrowed[24];memcpy(borrowed,command_topic,sizeof(command_topic)-1);memset(borrowed+sizeof(command_topic)-1,'X',sizeof(borrowed)-sizeof(command_topic)+1);
 MQTTString topic={.lenstring={sizeof(command_topic)-1,borrowed}};
 MQTTMessage message={binary,sizeof(binary),false};MessageData d={&topic,&message};
 assert(ingress_accept(&d,command)); /* Borrowed topic has no terminating NUL. */
 topic.lenstring.len--;assert(!ingress_accept(&d,command));
 topic.lenstring.len=sizeof(command_topic)-1;borrowed[2]=0;assert(!ingress_accept(&d,command));borrowed[2]=command_topic[2];
 message.retained=true;assert(!ingress_accept(&d,command));message.retained=false;
 topic.cstring="scope/device/ha/command";assert(!ingress_accept(&d,command));
 message.payload="{}";message.payloadlen=2;assert(ingress_accept(&d,command));
 topic.cstring="scope/device/command/extra";message.payload=binary;message.payloadlen=sizeof(binary);assert(!ingress_accept(&d,command));
 topic.cstring=NULL;topic.lenstring.len=-1;assert(!ingress_accept(&d,command));topic.lenstring.len=sizeof(command_topic)-1;
 topic.lenstring.data=NULL;assert(!ingress_accept(&d,command));topic.cstring=command_topic;
 message.payloadlen=MAX_MQTT_DATA_SIZE;assert(!ingress_accept(&d,command));message.payloadlen=0;assert(!ingress_accept(&d,command));
 message.payload="online";message.payloadlen=6;message.retained=true;assert(ingress_accept(&d,ha_birth));
 message.payload="onlineX";message.payloadlen=7;assert(!ingress_accept(&d,ha_birth));
 return 0;
}
''')
