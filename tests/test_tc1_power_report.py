"""TC1 owner cadence and real HA/Paho sends with a constrained heap."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_tc1_mqtt_stack import extract
from tests import test_shared_mqtt as shared

ROOT = shared.ROOT


class Tc1PowerReport(unittest.TestCase):
    def test_owner_cadence(self):
        source = (ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text()
        functions = '\n'.join(extract(source, signature) for signature in (
            'void UserMqttRefreshState(', 'static void power_report_poll(',
            'static bool power_requested(', 'static void ha_birth(',
            'static int session_initial(', 'static int session_cycle('))
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define MQTT_SUCCESS 0
#define PH_MQTT_RECONFIGURE 1
#define service_sender 0
static struct{int mode;char base[8];}account={0,"base"};
static struct{int mqtt_report_freq;}config={2},*user_config=&config;
static bool power_pending,power_turn,power_report_started,rediscover;
static bool power_sensor_metadata,power_sensor_started,cleanup_pending;
static bool current=true,ota_active;
static unsigned ha_refresh_pending,discovery_index=12,state_index=10,publication_cursor;
static uint32_t power_report_at,now;
static int synchronous_error;
static unsigned reports,invalidations,discovery_steps,state_steps;
typedef struct{void *payload;unsigned payloadlen;}MQTTMessage;
typedef struct{MQTTMessage *message;}MessageData;
static bool session_current(void){return current;}
static uint32_t mico_rtos_get_time(void){return now;}
static void PowerSampleInvalidate(void){invalidations++;}
static int service_mqtt_capabilities(int s,const void*a){(void)s;(void)a;return 0;}
static int ota_service(const char*b){(void)b;return 0;}
static bool ota_job_active(void){return ota_active;}
static int power_sensor_cycle(bool*h){*h=false;return 0;}
static bool service_mqtt_status_due(bool b){(void)b;return false;}
static int service_mqtt_snapshot(int s,const void*a){(void)s;(void)a;assert(0);return 0;}
static int snapshot_step(void){
 if(discovery_index<12){discovery_index++;discovery_steps++;}
 else if(state_index<10){if(state_index==9)reports++;state_index++;state_steps++;}
 return 0;
}
/* PRODUCTION */
static void cycle(unsigned expected){unsigned before=reports;assert(!session_cycle(0));assert(reports==before+expected);}
static void drain(void){for(unsigned i=0;i<22;i++)cycle(i==21);assert(discovery_index==12&&state_index==10);}
int main(void){
 /* A due report is coalesced on one owner turn; tight polling adds no copies. */
 cycle(1);for(unsigned i=0;i<100;i++)cycle(0);
 now=1999;cycle(0);now=2000;cycle(1);cycle(0);
 /* Changes take effect on the next turn without waiting for the old sleep. */
 config.mqtt_report_freq=10;now=11999;cycle(0);now=12000;cycle(1);
 config.mqtt_report_freq=1;now=13000;cycle(1);
 config.mqtt_report_freq=0;now=14999;cycle(0);now=15000;cycle(1);
 config.mqtt_report_freq=-1;now=17000;cycle(1);
 /* Factory/corrupt sentinels keep a bounded two-second fallback. */
 config.mqtt_report_freq=0x7fffffff;now=19000;cycle(1);
 config.mqtt_report_freq=3600;now=3618999;cycle(0);now++;cycle(1);
 /* Unsigned monotonic subtraction survives the 32-bit millisecond wrap. */
 config.mqtt_report_freq=2;power_report_started=true;power_report_at=UINT32_MAX-999u;
 now=999;cycle(0);now=1000;cycle(1);
 /* An overdue owner coalesces one report instead of replaying missed ticks. */
 now+=300000;cycle(1);cycle(0);
 /* Disconnected/reconfigured and OTA sessions do no periodic work. */
 current=false;unsigned before=reports;now+=2000;
 assert(session_cycle(0)==PH_MQTT_RECONFIGURE&&reports==before);
 current=true;ota_active=true;cycle(0);ota_active=false;cycle(1);
 /* Reconnection resets cadence and preserves the ordinary initial snapshot. */
 assert(!session_initial(0)&&invalidations==1&&!power_report_started);
 drain();cycle(1);cycle(0);
 /* Birth and control refreshes still enter the complete snapshot path. */
 MQTTMessage m={"online",6};MessageData d={&m};ha_birth(&d);drain();cycle(0);
 UserMqttRefreshState(false);for(unsigned i=0;i<10;i++)cycle(i==9);
 UserMqttRefreshState(true);drain();cycle(0);
 assert(discovery_steps==36&&state_steps==reports+36);
 return 0;
}
'''.replace('/* PRODUCTION */', functions)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path/'cadence.c').write_text(fixture)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra',
                '-Werror', '-fsanitize=address,undefined', '-no-pie', '-I'+str(ROOT),
                str(path/'cadence.c'), '-o', str(path/'cadence')], check=True)
            subprocess.run([str(path/'cadence')], check=True, timeout=10)

    def test_real_power_callees_under_heap_pressure(self):
        source = (ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text()
        # The permanent footprint comes from the real application startup.
        # A reintroduced periodic thread must be charged by this regression.
        startup = (ROOT/'TC1/main.c').read_text()
        threads = re.findall(r'mico_rtos_create_thread\([^;]+;', startup)
        periodic_stack = sum(int(re.search(r',\s*(0x[0-9a-fA-F]+)\s*,\s*0\)', t)[1], 16)
            for t in threads if 'mqtt_power_report' in t)
        functions = '\n'.join(extract(source, signature) for signature in (
            'static void power_report_poll(', 'static bool power_requested(',
            'static OSStatus MqttMsgPublish(Client *c,',
            'static int send_owned(', 'OSStatus UserMqttSendTopic(',
            'static bool power_number(', 'extern void UserMqttHassPower(void) {',
            'static int snapshot_step(', 'static int session_cycle('))
        # Discovery/state encoders precede the periodic branch and are covered
        # by cadence tests. Execute the actual periodic snapshot enumerator,
        # HA formatter, product sender, shared owner and pinned Paho here.
        names = ['UserMqttHassAutoLed', 'UserMqttHassAutoTotalSocket',
            'UserMqttHassAutoChildLock', 'UserMqttHassAutoRebootButton',
            'UserMqttHassAutoSoftRebootButton', 'UserMqttHassAutoPower',
            'UserMqttSendLedState', 'UserMqttSendTotalSocketState',
            'UserMqttSendChildLockState']
        declarations = '\n'.join('static void '+name+'(void){assert(0);}' for name in names)
        fixture = r'''
#define OVERLOAD_FIXTURE 1
#define main fixture_main
#include "tests/mqtt_core/paho.c"
#undef main
#include "common/ha_json.h"
#include "common/a1_decimal.inc"
#include "common/memory_budget.h"
#define VERSION "rc4"
#define Station 0
#define kUnknownErr -1
#define kConnectionErr -2
#define kParamErr -3
#define MAX_MQTT_TOPIC_SIZE MQTT_TOPIC_MAX
#define MAX_MQTT_DATA_SIZE 2048
#define require(c,label) do{if(!(c))goto label;}while(0)
#define c client
typedef int OSStatus;
typedef uint32_t mico_time_t;
typedef struct{char ip[16];}IPStatusTypedef;
static int micoWlanGetIPStatus(IPStatusTypedef*p,int s){(void)s;strcpy(p->ip,"10.1.1.1");return 0;}
static void mico_time_get_time(mico_time_t*p){*p=now_ms;}
static struct{int mqtt_report_freq;uint32_t p_count_1_day_ago,p_count_2_days_ago;}config={2,0,0},*user_config=&config;
static bool isconnect=true,synchronous_send,power_pending,power_turn,power_report_started,rediscover;
static uint32_t power_report_at,p_count=10000,power_session_pulses=2000;
static double real_time_power=123;
static unsigned ha_refresh_pending,discovery_index=12,state_index=10,publication_cursor,publication_seen;
static bool publication_turn,publication_sent;
static int synchronous_error;
static double PowerPulseCountToKwh(uint32_t n){return n/1000.0;}
static bool session_current(void){return true;}
static bool ota_job_active(void){return false;}
static int ota_service(const char*b){(void)b;return 0;}
static int power_sensor_cycle(bool*h){*h=false;return 0;}
static bool service_mqtt_status_due(bool changed){(void)changed;return false;}
#define service_sender 0
static int service_mqtt_snapshot(int s,const void*a){(void)s;(void)a;assert(0);return 0;}
static void UserMqttHassAuto(unsigned n){(void)n;assert(0);}
static void UserMqttSendSocketState(unsigned n){(void)n;assert(0);}
static unsigned states,sensors,receipts;
static int UserMqttSendTopic(char*,char*,char);
extern void UserMqttHassPower(void);
/* DECLARATIONS */
/* PRODUCTION */
#undef c
static void overload_handler(MessageData*d){
 assert(d->message->payloadlen==2&&!memcmp(d->message->payload,"{}",2));
 assert(ph_mqtt_input_consumed());executions++;
 mqtt_target_t target;mqtt_target_set(&target,&account);
 static const unsigned char receipt[]={0x81,0xa4,'c','o','d','e',0xa8,'a','c','c','e','p','t','e','d'};
 assert(!ph_mqtt_receipt_send_bytes(&target,receipt,sizeof(receipt),NULL,NULL));
}
static void overload_publication(const unsigned char*t,unsigned tn,const unsigned char*b,size_t n){
 if(tn>=9&&!memcmp(t+tn-9,"/ha/state",9)){assert(n>2&&b[0]=='{');states++;}
 else if(tn>=10&&!memcmp(t+tn-10,"/ha/sensor",10)){assert(n>2&&b[0]=='{');sensors++;}
 else{assert(tn>=7&&!memcmp(t+tn-7,"/result",7)&&n==15);receipts++;}
}
static void turn(void){ph_mqtt_turn_begin(4);int rc=session_cycle(NULL);ph_mqtt_turn_end();assert(rc==MQTT_SUCCESS||rc==PH_MQTT_MORE);ph_mqtt_resource_round_end(true);}
int main(void){
 assert(mqtt_account_init(&account,1,"tc1","test","aabbccddeeff"));
 assert(ph_mqtt_owner_init(&account,8192,&thread,accept));init();
 /* Model an RX/HTTP burst at the live reserve boundary. The retired 4KiB
  * stack is the only difference; the common reserve is kept at 16KiB. */
 size_t baseline=charged_live;
 heap_capacity=baseline+SERVICE_MEMORY_RESERVE+4096u+64u;
 void *retired=malloc(4096);assert(retired);
 service_memory_state_t memory={.minimum_free=UINT32_MAX};
 unsigned old_free=MicoGetMemoryInfo()->free_memory;
 assert(!memory_budget_claim(&memory,MicoGetMemoryInfo()->free_memory,100));
 turn();assert(!states&&!sensors&&state_index==9);
 free(retired);
 now_ms+=1000; /* Preserve the shared owner's resource retry cadence. */
 void *periodic=PERIODIC_STACK?malloc(PERIODIC_STACK):NULL;
 assert(!PERIODIC_STACK||periodic);
 unsigned new_free=MicoGetMemoryInfo()->free_memory;
 assert(new_free>=old_free+4096u);
 unsigned claim=memory_budget_claim(&memory,MicoGetMemoryInfo()->free_memory,100);
 assert(claim);memory_budget_release(&memory,claim);
 /* Resume the same production publication. One packet per business turn,
  * both HA topics once, without allocating or running another producer. */
 turn();assert(states==1&&!sensors&&state_index==9);
 /* A command received between the two periodic packets retains its result
  * turn; the publication cursor resumes without replaying the first topic. */
 incoming("rx/command","{}",99);assert(!ph_mqtt_yield(&client,1));
 assert(!ph_mqtt_poll(&client));assert(executions==1&&receipts==1);
 turn();assert(states==1&&sensors==1&&state_index==10);
 turn();assert(states==1&&sensors==1);
 now_ms+=2000;for(unsigned i=0;i<3;i++)turn();
 assert(states==2&&sensors==2&&receipts==1);
 free(periodic);heap_capacity=40000;
 cleanup();ph_mqtt_owner_end();assert(!native_live&&!charged_live);
 printf("PASS TC1 owner cadence -> snapshot -> real HA sender -> pinned Paho: free %u -> %u, reserve %u, HA packets %u/%u, interleaved receipts %u\n",old_free,new_free,SERVICE_MEMORY_RESERVE,states,sensors,receipts);
 return 0;
}
'''.replace('/* DECLARATIONS */', declarations).replace('/* PRODUCTION */', functions)
        fixture = fixture.replace('PERIODIC_STACK', str(periodic_stack))
        harness = shared.SharedMQTT()
        with tempfile.TemporaryDirectory() as directory, harness.protocol('TC1') as protocol:
            probe = Path(directory)/'pressure.c'
            probe.write_text(fixture)
            harness.compile_run([probe, ROOT/'common/mqtt/core.c', ROOT/'common/mqtt/client.c',
                ROOT/'common/mqtt/session.c', protocol/'MQTTClient.c',
                *sorted((protocol/'MQTTPacket').glob('*.c'))],
                [ROOT, ROOT/'tests/mqtt_core/stubs', protocol, protocol/'mico', protocol/'MQTTPacket'],
                ['-DPHICOMM_BINARY_MQTT=1',
                 '-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])


if __name__ == '__main__':
    unittest.main()
