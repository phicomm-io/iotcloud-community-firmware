"""Native MessagePack transport borrowing, NUL and retry coverage."""
from pathlib import Path
import tempfile
import unittest
from tests import test_shared_mqtt as shared
ROOT=shared.ROOT

class NativeMqtt(unittest.TestCase):
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
