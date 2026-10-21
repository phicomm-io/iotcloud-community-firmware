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
            'static void ha_birth(', 'static bool ingress_accept(',
            'static __attribute__((noinline)) bool tc1_command_topic(',
            'static __attribute__((noinline)) void tc1_ota_message(',
            'static void MessageArrived(MessageData *md) {'))
        fixture=r'''
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "common/mqtt_account.h"
#define MAX_MQTT_DATA_SIZE 2048
#define service_sender NULL
static bool isconnect=true,current=true,ota_active,rediscover,stopping;
static unsigned commands,otas,legacy;
static mqtt_account_t account;
static char ota_topic[512],topic_set[]="legacy";
typedef struct {void *payload;unsigned payloadlen;bool retained;unsigned qos;} MQTTMessage;
typedef struct {const char *cstring;struct{const char *data;unsigned len;}lenstring;} MQTTString;
typedef struct {MQTTMessage *message;MQTTString *topicName;} MessageData;
typedef void (*messageHandler)(MessageData *);
static bool session_current(void){return current;}
static bool session_stop(void*x){(void)x;return stopping;}
static bool ota_job_active(void){return ota_active;}
static int service_mqtt_input(void *sender,const mqtt_account_t *scope,const void *data,size_t n,bool retained){assert(!sender&&scope==&account&&!retained&&n==2&&!memcmp(data,"{}",2));commands++;return 0;}
static void ota_command(const char *owner,const void *data,size_t n,bool retained){assert(owner==account.base&&!retained&&n==2&&!memcmp(data,"{}",2));otas++;}
static void ProcessHaCmd(char *p){assert(!strcmp(p,"{}"));legacy++;}
static void MessageArrived(MessageData *);
'''+functions+r'''
int main(void){
 assert(mqtt_account_init(&account,1,"tc1","test","aabbccddeeff"));
 char command[512];assert(mqtt_leaf(&account,"command",command,sizeof(command)));assert(mqtt_leaf(&account,"ota/set",ota_topic,sizeof(ota_topic)));
 MQTTMessage message={"{}",2,false,1};MQTTString topic={command,{command,strlen(command)}};MessageData data={&message,&topic};
 assert(ingress_accept(&data,MessageArrived));MessageArrived(&data);assert(commands==1&&!otas&&!legacy);
 message.retained=true;assert(!ingress_accept(&data,MessageArrived));MessageArrived(&data);assert(commands==1);message.retained=false;
 current=false;MessageArrived(&data);assert(commands==1);current=true;
 /* CONNECTING is a live subscribed session, despite public READY=false. */
 isconnect=false;MessageArrived(&data);assert(commands==2);isconnect=true;
 stopping=true;MessageArrived(&data);assert(commands==2);stopping=false;
 message.payload="\0x";assert(!ingress_accept(&data,MessageArrived));message.payload="{}";
 message.payloadlen=2048;assert(!ingress_accept(&data,MessageArrived));message.payloadlen=2;
 topic.cstring=ota_topic;MessageArrived(&data);assert(otas==1&&commands==2);
 topic.cstring=topic_set;ota_active=true;MessageArrived(&data);assert(!legacy);ota_active=false;
 MessageArrived(&data);assert(legacy==1);
 message.payload="online";message.payloadlen=6;message.retained=true;
 assert(ingress_accept(&data,ha_birth));ha_birth(&data);assert(rediscover);
 assert(!ingress_accept(&data,MessageArrived));
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
        power=extract(source,'static bool power_requested(')+'\n'+extract(source,'extern void UserMqttHassPower(void) {')
        fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include "common/mqtt_account.h"
static mqtt_account_t account;
static bool isconnect=true,synchronous_send,power_pending;
static __thread bool owner;
static bool ph_mqtt_owner(void){return owner;}
static uint32_t p_count=10000,power_session_pulses=2000;
static double real_time_power=123;
static struct {uint32_t p_count_1_day_ago,p_count_2_days_ago;} cfg={9000,8000},*user_config=&cfg;
static char str_mac[]="aabbccddeeff",topic_state[MQTT_TOPIC_MAX];
typedef uint32_t mico_time_t;
static void mico_time_get_time(mico_time_t *out){*out=93784000;}
static double PowerPulseCountToKwh(uint32_t n){return n/1000.0;}
static char topics[6][MQTT_TOPIC_MAX],payloads[6][512];static unsigned count;static int retained[6];
static int UserMqttSendTopic(char*topic,char*payload,char retain){assert(owner&&count<6);strcpy(topics[count],topic);strcpy(payloads[count],payload);retained[count++]=retain;return 0;}
'''+power+r'''
static void *producer(void*x){(void)x;for(unsigned i=0;i<10000;i++){UserMqttHassPower();sched_yield();}return NULL;}
int main(void){
 owner=true;
 assert(mqtt_account_init(&account,1,"tc1","test",str_mac));assert(mqtt_leaf(&account,"sensor",topic_state,sizeof(topic_state)));
 UserMqttHassPower();assert(count==1&&!strcmp(topics[0],topic_state)&&retained[0]==1);
 assert(!strcmp(payloads[0],"{\"power\":12.300,\"energySession\":2.000000,\"powerConsumption\":10.000,\"powerConsumptionToday\":1.000,\"powerConsumptionYesterday\":1.000,\"startupTime\":\"1 - 02:03:04\"}"));
 count=0;account.mode=0;UserMqttHassPower();assert(count==5);
 const char*leaves[]={"power","powerConsumption","startupTime","powerConsumptionToday","powerConsumptionYesterday"};
 for(unsigned i=0;i<5;i++){char expected[128];snprintf(expected,sizeof(expected),"homeassistant/sensor/%s/%s/state",str_mac,leaves[i]);assert(!strcmp(topics[i],expected)&&!retained[i]);}
 isconnect=false;count=0;UserMqttHassPower();assert(!count);
 synchronous_send=true;UserMqttHassPower();assert(count==5);
 pthread_t thread;assert(!pthread_create(&thread,NULL,producer,NULL));
 /* Owner can rename/rebuild topics while the external worker only coalesces
  * typed work. It never borrows or encodes an old session namespace. */
 for(unsigned i=0;i<500;i++){
  account.mode=i%2;strcpy(topic_state,i%2?"current/sensor":"next/sensor");
  count=0;UserMqttHassPower();assert(count==(account.mode?1:5));
 }
 assert(!pthread_join(thread,NULL));assert(power_requested());assert(!power_requested());
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp);probe=path/'power.c';probe.write_text(fixture)
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g','-pthread','-fsanitize='+os.environ.get('PHICOMM_MQTT_SANITIZER','address,undefined'),'-fno-omit-frame-pointer','-no-pie','-I'+str(ROOT),str(probe),'-o',str(path/'power')],check=True)
            subprocess.run([str(path/'power')],check=True,timeout=10)


if __name__=='__main__':unittest.main()
