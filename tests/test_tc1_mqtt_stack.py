"""Execute real TC1 topic/OTA/routing functions after splitting branch scratch."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def extract(source, signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]


class Tc1MqttStack(unittest.TestCase):
    def test_routing_and_ownership(self):
        source=(ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text()
        functions='\n'.join(extract(source,s) for s in (
            'static __attribute__((noinline)) bool tc1_command_topic(',
            'static __attribute__((noinline)) void tc1_ota_message(',
            'static void MessageArrived(MessageData *md) {'))
        fixture=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "common/mqtt_account.h"
#define MAX_MQTT_TOPIC_SIZE 512
#define MAX_MQTT_DATA_SIZE 2048
#define kUnknownErr -1
#define kNoMemoryErr -2
#define kNoErr 0
#define require_action(c,l,a) do{if(!(c)){a;goto l;}}while(0)
#define require_noerr(c,l) do{if(c)goto l;}while(0)
#define mqtt_log(...) ((void)0)
#define service_sender NULL
static bool isconnect=true,current=true,ota_active,fail_async,fail_alloc;
static unsigned commands,otas,allocated,generation=19,mqtt_client_worker_thread;
static mqtt_account_t account;
static char ota_topic[512];
typedef int OSStatus;
typedef struct {void *payload;unsigned payloadlen;bool retained;unsigned qos;} MQTTMessage;
typedef struct {const char *cstring;struct{const char *data;unsigned len;}lenstring;} MQTTString;
typedef struct {MQTTMessage *message;MQTTString *topicName;} MessageData;
typedef struct {char topic[512];uint32_t generation;char qos,retained;char data[2048];uint32_t datalen;} mqtt_recv_msg_t,*p_mqtt_recv_msg_t;
static p_mqtt_recv_msg_t queued;
static bool session_current(void){return current;}
static bool ota_job_active(void){return ota_active;}
static int UserRecvHandler(void *p){(void)p;return 0;}
static void *allocate(size_t n,size_t size){if(fail_alloc)return NULL;void*p=calloc(n,size);if(p)allocated++;return p;}
static void release(void *p){if(p){assert(allocated);allocated--;free(p);}}
static int mico_rtos_send_asynchronous_event(unsigned *worker,int(*cb)(void*),void *p){assert(worker==&mqtt_client_worker_thread&&cb==UserRecvHandler);if(fail_async)return -1;assert(!queued);queued=p;return 0;}
static int service_mqtt_input(void *sender,const mqtt_account_t *scope,const void *data,size_t n,bool retained){assert(!sender&&scope==&account&&!retained&&n==2&&!memcmp(data,"{}",2));commands++;return 0;}
static void ota_command(const char *owner,const void *data,size_t n,bool retained){assert(owner==account.base&&!retained&&n==2&&!memcmp(data,"{}",2));otas++;}
#define calloc allocate
#define free release
'''+functions+r'''
int main(void){
 assert(mqtt_account_init(&account,1,"tc1","test","aabbccddeeff"));
 char command[512];assert(mqtt_leaf(&account,"command",command,sizeof(command)));assert(mqtt_leaf(&account,"ota/set",ota_topic,sizeof(ota_topic)));
 MQTTMessage message={"{}",2,false,1};MQTTString topic={command,{command,strlen(command)}};MessageData data={&message,&topic};
 MessageArrived(&data);assert(commands==1&&!otas&&!allocated);
 topic.cstring=NULL;MessageArrived(&data);assert(commands==2);
 message.retained=true;MessageArrived(&data);assert(commands==2);message.retained=false;
 current=false;MessageArrived(&data);assert(commands==2);current=true;
 isconnect=false;MessageArrived(&data);assert(commands==2);isconnect=true;
 message.payload="\0x";MessageArrived(&data);assert(commands==2);message.payload="{}";
 topic.cstring=ota_topic;topic.lenstring.data=ota_topic;topic.lenstring.len=strlen(ota_topic);MessageArrived(&data);assert(otas==1&&commands==2);
 topic.cstring="legacy";topic.lenstring.data="legacy";topic.lenstring.len=6;
 ota_active=true;MessageArrived(&data);assert(!allocated);ota_active=false;
 MessageArrived(&data);assert(queued&&allocated==1&&queued->generation==generation&&queued->datalen==2&&!strcmp(queued->data,"{}")&&!strcmp(queued->topic,"legacy"));release(queued);queued=NULL;
 fail_async=true;MessageArrived(&data);assert(!allocated&&!queued);fail_async=false;
 fail_alloc=true;MessageArrived(&data);assert(!allocated&&!queued);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp);probe=path/'probe.c';probe.write_text(fixture)
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g','-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),str(probe),'-o',str(path/'probe')],check=True)
            subprocess.run([str(path/'probe')],check=True,timeout=30)

    def test_power_topic_and_payload(self):
        source=(ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text()
        power=extract(source,'extern void UserMqttHassPower(void) {')
        fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "common/mqtt_account.h"
static mqtt_account_t account;
static bool isconnect=true,synchronous_send;
static uint32_t p_count=10000,power_session_pulses=2000;
static double real_time_power=123;
static struct {uint32_t p_count_1_day_ago,p_count_2_days_ago;} cfg={9000,8000},*user_config=&cfg;
static char str_mac[]="aabbccddeeff",topic_state[MQTT_TOPIC_MAX];
typedef uint32_t mico_time_t;
static void mico_time_get_time(mico_time_t *out){*out=93784000;}
static double PowerPulseCountToKwh(uint32_t n){return n/1000.0;}
static char topics[6][MQTT_TOPIC_MAX],payloads[6][512];static unsigned count;static int retained[6];
static int UserMqttSendTopic(char*topic,char*payload,char retain){assert(count<6);strcpy(topics[count],topic);strcpy(payloads[count],payload);retained[count++]=retain;return 0;}
'''+power+r'''
int main(void){
 assert(mqtt_account_init(&account,1,"tc1","test",str_mac));assert(mqtt_leaf(&account,"sensor",topic_state,sizeof(topic_state)));
 UserMqttHassPower();assert(count==1&&!strcmp(topics[0],topic_state)&&retained[0]==1);
 assert(!strcmp(payloads[0],"{\"power\":12.300,\"energySession\":2.000000,\"powerConsumption\":10.000,\"powerConsumptionToday\":1.000,\"powerConsumptionYesterday\":1.000,\"startupTime\":\"1 - 02:03:04\"}"));
 count=0;account.mode=0;UserMqttHassPower();assert(count==5);
 const char*leaves[]={"power","powerConsumption","startupTime","powerConsumptionToday","powerConsumptionYesterday"};
 for(unsigned i=0;i<5;i++){char expected[128];snprintf(expected,sizeof(expected),"homeassistant/sensor/%s/%s/state",str_mac,leaves[i]);assert(!strcmp(topics[i],expected)&&!retained[i]);}
 isconnect=false;count=0;UserMqttHassPower();assert(!count);
 synchronous_send=true;UserMqttHassPower();assert(count==5);return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp);probe=path/'power.c';probe.write_text(fixture)
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),str(probe),'-o',str(path/'power')],check=True)
            subprocess.run([str(path/'power')],check=True,timeout=10)


if __name__=='__main__':unittest.main()
