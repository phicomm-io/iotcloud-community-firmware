"""TC1 production Init/Stop/worker with real creator/worker/observer threads."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_tc1_mqtt_stack import extract
ROOT=Path(__file__).resolve().parents[1]
class TC1Lifecycle(unittest.TestCase):
    def test_actual_lifecycle(self):
        source=(ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text()
        client=(ROOT/'common/mqtt/client.c').read_text()
        functions='\n'.join(extract(source,s) for s in ('OSStatus UserMqttDeInit(',
            'OSStatus UserMqttInit(', 'static bool session_stop(',
            'void MqttClientThread(mico_thread_arg_t arg) {','bool UserMqttIsConnect('))
        startup='\n'.join(extract(client,s) for s in ('uint32_t ph_mqtt_startup_prepare(',
            'void ph_mqtt_startup_publish(', 'bool ph_mqtt_startup_wait('))
        fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include "client.h"
#include "session.h"
typedef int OSStatus;
typedef void (*mico_thread_function_t)(mico_thread_arg_t);
#define MICO_APPLICATION_PRIORITY 0
#define MAX_MQTT_DATA_SIZE 2048
#define MQTT_CLIENT_SUB_TOPIC1 "set/%s"
#define MQTT_CLIENT_PUB_TOPIC "state/%s"
#define mqtt_log(...) ((void)0)
#define TC1_STATUS_WRITE(s) do{s;}while(0)
#define MQTT_CONFIG_ERROR 99
#define MQTT_THREAD_RUNNING 1u
#define MQTT_THREAD_STOP 2u
static bool isconnect,done,entered;
static unsigned mqtt_lifecycle,tc1_mqtt_status,created,ended;
static char topic_set[64],topic_state[64],str_mac[]="aabbccddeeff";
static mico_thread_t mqtt_owner_thread;
static ph_mqtt_startup mqtt_startup;
static mqtt_account_t account;
static Client c;
static bool create_fail;
static pthread_t worker_thread;
static void MqttClientThread(mico_thread_arg_t);
uint32_t MQTTClientLibVersion(void){return 1;}
static bool ingress_accept(MessageData*d,messageHandler h){(void)d;(void)h;return true;}
static int outgoing(const ph_mqtt_item*i){(void)i;return 0;}
static int session_prepare(void*x,ph_mqtt_connection*y){(void)x;(void)y;return 0;}
static bool session_subscription(void*x,unsigned i,ph_mqtt_subscription*y){(void)x;(void)i;(void)y;return false;}
static int session_initial(void*x){(void)x;return 0;}
static int session_cycle(void*x){(void)x;return 0;}
static void session_closing(void*x,bool b,int rc){(void)x;(void)b;(void)rc;}
static void session_event(void*x,unsigned e,int rc){(void)x;(void)e;(void)rc;}
static bool session_initial_ready(void*x){(void)x;return true;}
static bool session_valid(void*x){(void)x;return true;}
static bool session_stop(void *);
uint32_t mico_rtos_get_time(void){static unsigned t;return __atomic_add_fetch(&t,1,__ATOMIC_RELAXED);}
void mico_thread_msleep(unsigned ms){(void)ms;sched_yield();}
bool ph_mqtt_owner_init(const mqtt_account_t*a,size_t n,mico_thread_t*t,bool(*f)(MessageData*,messageHandler)){(void)a;(void)n;(void)t;(void)f;return true;}
void ph_mqtt_outgoing(int(*f)(const ph_mqtt_item*)){(void)f;}
void ph_mqtt_owner_end(void){__atomic_add_fetch(&ended,1,__ATOMIC_RELAXED);}
void ph_mqtt_session_run(Client*p,const ph_mqtt_session_hooks*h){
 (void)p;__atomic_store_n(&isconnect,true,__ATOMIC_RELEASE);
 __atomic_store_n(&entered,true,__ATOMIC_RELEASE);
 while(!h->stop(NULL))sched_yield();
}
void mico_rtos_delete_thread(void*x){assert(!x);}
static void *entry(void *arg){MqttClientThread((mico_thread_arg_t)arg);return NULL;}
static int mico_rtos_create_thread(mico_thread_t*out,unsigned priority,const char*name,
 mico_thread_function_t fn,unsigned bytes,mico_thread_arg_t arg){
 (void)priority;(void)name;assert(fn==MqttClientThread&&bytes==8192);created++;
 if(create_fail)return -1;
 assert(!pthread_create(&worker_thread,NULL,entry,(void*)arg));
 /* Entry can run before the RTOS publishes its output handle. */
 sched_yield();*out=123;return 0;
}
/* STARTUP */
/* PRODUCTION */
static void *observer(void*x){(void)x;while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE)){
 (void)UserMqttIsConnect();(void)session_stop(NULL);sched_yield();}return NULL;}
int main(void){
 pthread_t reader;assert(!pthread_create(&reader,NULL,observer,NULL));
 for(unsigned i=0;i<500;i++){
  create_fail=i%10==0;__atomic_store_n(&entered,false,__ATOMIC_RELEASE);
  unsigned before=created;assert(UserMqttInit()==(create_fail?-1:0));assert(created==before+1);
  if(create_fail){assert(!__atomic_load_n(&mqtt_lifecycle,__ATOMIC_ACQUIRE));continue;}
  while(!__atomic_load_n(&entered,__ATOMIC_ACQUIRE))sched_yield();
  assert(UserMqttInit()==0&&created==before+1); /* Never a duplicate worker. */
  assert(UserMqttDeInit()==0&&!UserMqttIsConnect());
  assert(!pthread_join(worker_thread,NULL));
  assert(!__atomic_load_n(&mqtt_lifecycle,__ATOMIC_ACQUIRE)&&!UserMqttIsConnect());
 }
 __atomic_store_n(&done,true,__ATOMIC_RELEASE);assert(!pthread_join(reader,NULL));
 assert(created==500&&ended==450);puts("TC1 actual Init/Stop/worker: 500 cycles, create failure, concurrent observer, no duplicate worker PASS");
 return 0;
}
'''.replace('/* STARTUP */',startup).replace('/* PRODUCTION */',functions)
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'probe.c').write_text(fixture)
            sdk=ROOT/'build/tc1-sdk-3.6.4/mico-os/libraries/protocols/mqtt'
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-pthread','-g',
                '-fsanitize='+os.environ.get('PHICOMM_MQTT_SANITIZER','address,undefined'),'-fno-pie','-no-pie',
                '-I'+str(ROOT/'tests/mqtt_core/stubs'),'-I'+str(ROOT/'common/mqtt'),
                '-I'+str(sdk),'-I'+str(sdk/'mico'),'-I'+str(sdk/'MQTTPacket'),
                str(p/'probe.c'),'-o',str(p/'probe')],check=True)
            subprocess.run([str(p/'probe')],check=True,timeout=30)
