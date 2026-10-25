"""Real protocol and service ownership under preflight-only heap pressure."""
import unittest
from pathlib import Path
import tempfile
from tests import test_shared_mqtt as shared
class Preflight(unittest.TestCase):
    def test_protocol_ownership_and_scheduler(self):
        harness=shared.SharedMQTT()
        for model in ('A1','M1','TC1'):
            with self.subTest(model=model):harness.check_session_fixture('preflight',model)
    def test_receipt_ownership(self):
        shared.SharedMQTT().check_service_fixture(extra_defines=['-DPREFLIGHT_FIXTURE'])
    def test_tc1_native_cursor_and_owned_outcome(self):
        from tests.test_http_json_budget import function
        source=(shared.ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text()
        body='\n'.join(function(source,s) for s in ('static OSStatus MqttMsgPublish(', 'OSStatus UserMqttSendTopic('))
        fixture=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int OSStatus;typedef int Client;
enum QoS{QOS0,QOS1};
typedef struct{enum QoS qos;int retained;void*payload;unsigned payloadlen;} MQTTMessage;
#define MQTTMessage_publishData_initializer {0}
#define require(ok,label) do{if(!(ok))goto label;}while(0)
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 2
#define PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED 3
#define MQTT_SOCKET_ERR -3
#define kNoErr 0
#define kUnknownErr -1
#define kConnectionErr -2
#define kParamErr -4
#define MAX_MQTT_TOPIC_SIZE 512
#define MAX_MQTT_DATA_SIZE 2048
static int sdk_rc,c,synchronous_error;
static bool publication_turn=true,publication_sent,synchronous_send=true;
static unsigned publication_seen,publication_cursor;
static bool ph_mqtt_owner(void){return true;}
static int ph_mqtt_publish(Client*p,const char*t,MQTTMessage*m){(void)p;(void)t;(void)m;return sdk_rc;}
static int ph_mqtt_enqueue_send(const char*t,const char*b,unsigned n,int q,int r){(void)t;(void)b;(void)n;(void)q;(void)r;assert(0);return -1;}
static OSStatus MqttMsgPublish(Client*,const char*,char,char,const unsigned char*,uint32_t);
static int send_owned(const char*t,const char*b,bool r){return MqttMsgPublish(&c,t,QOS1,r,(const unsigned char*)b,strlen(b));}
/* BODY */
int main(void){
 sdk_rc=3;assert(MqttMsgPublish(&c,"t",QOS1,1,(const unsigned char*)"body",4)==3);
 assert(UserMqttSendTopic("t","body",1)==2&&!publication_cursor&&!publication_sent);
 publication_seen=0;sdk_rc=0;
 assert(!UserMqttSendTopic("t","updated",1)&&publication_cursor==1&&publication_sent);
 sdk_rc=MQTT_SOCKET_ERR;assert(MqttMsgPublish(&c,"t",QOS1,1,(const unsigned char*)"body",4)==kConnectionErr);
}
'''.replace('/* BODY */',body)
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp,'adapter.c');path.write_text(fixture)
            shared.SharedMQTT().compile_run([path])
    def test_tc1_initial_retry_state(self):
        from tests.test_http_json_budget import function
        body=function((shared.ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text(),
                      'static int session_initial(')
        fixture=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define MQTT_SUCCESS 0
static struct {unsigned mode;} account={1};
static bool cleanup_pending;
static unsigned discovery_index,state_index,publication_cursor,snapshot_at;
static unsigned invalidations;
static void PowerSampleInvalidate(void){invalidations++;}
static int synchronous_error,cap_rc;
static uint32_t mico_rtos_get_time(void){return 100;}
static int service_sender(const char*t,const char*b,bool r){(void)t;(void)b;(void)r;return 0;}
static int service_mqtt_capabilities(int(*s)(const char*,const char*,bool),void*a){
 (void)s;(void)a;
 /* A retryable first capability send must already have installed all cursors. */
 assert(cleanup_pending&&!discovery_index&&!state_index&&!publication_cursor&&!synchronous_error);
 assert((uint32_t)(100-snapshot_at)==5000);return cap_rc;
}
/* BODY */
int main(void){
 for(cap_rc=0;cap_rc<=3;cap_rc++){
  cleanup_pending=false;discovery_index=state_index=publication_cursor=9;synchronous_error=-1;
  assert(session_initial(0)==cap_rc);
  assert(invalidations==(unsigned)cap_rc+1);
 }
}
'''.replace('/* BODY */',body)
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp,'initial.c');path.write_text(fixture)
            shared.SharedMQTT().compile_run([path])
