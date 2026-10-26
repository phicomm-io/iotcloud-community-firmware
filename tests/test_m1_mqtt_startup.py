"""M1 worker startup with the real owner binding and pinned Paho session."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_http_json_budget import function
from tests import test_shared_mqtt as shared

ROOT = Path(__file__).resolve().parents[1]


class M1MqttStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.temp.name)
        cls.protocol_context = shared.SharedMQTT().protocol('M1')
        cls.protocol = cls.protocol_context.__enter__()
        product = (ROOT / 'M1/mqtt.c').read_text()
        cls.source = cls.fixture(product)
        cls.binary = cls.path / 'startup'
        cls.build(cls.source, cls.binary)

    @classmethod
    def tearDownClass(cls):
        cls.protocol_context.__exit__(None, None, None)
        cls.temp.cleanup()

    @staticmethod
    def fixture(product):
        worker = function(product, 'static void mqtt_worker(')
        start = function(product, 'OSStatus m1_mqtt_start(')
        return r'''
#define main existing_paho_scenarios
#include "tests/mqtt_core/paho.c"
#undef main
typedef int OSStatus;
#define MICO_APPLICATION_PRIORITY 0
#define custom_log(...) ((void)0)
static mico_thread_t mqtt_owner_thread;
static ph_mqtt_startup mqtt_startup;
static unsigned created,deleted,owner_attempts,owner_failures,session_runs,status;
static bool create_fail;
static void (*pending_worker)(mico_thread_arg_t);
static mico_thread_arg_t pending_argument;
static void mqtt_status(unsigned s){status=s;}
static bool ingress_accept(MessageData *d,messageHandler h){return accept(d,h);}
static bool session_initial_ready(void *ctx){(void)ctx;return true;}
static bool session_valid(void *ctx){(void)ctx;return true;}
static void mico_rtos_delete_thread(void *t){assert(!t);deleted++;}
static int mico_rtos_create_thread(mico_thread_t *out,unsigned priority,const char *name,
 void (*worker)(mico_thread_arg_t),unsigned bytes,mico_thread_arg_t arg){
 (void)priority;assert(!strcmp(name,"m1_mqtt")&&bytes==16384);created++;
 if(create_fail)return -1;
 assert(!pending_worker);*out=current_thread;pending_worker=worker;pending_argument=arg;
 return 0;
}
int __real_mico_rtos_init_mutex(mico_mutex_t *);
int __wrap_mico_rtos_init_mutex(mico_mutex_t *m){
 owner_attempts++;
 if(owner_failures){owner_failures--;return -1;}
 return __real_mico_rtos_init_mutex(m);
}
void __real_ph_mqtt_session_run(Client *,const ph_mqtt_session_hooks *);
void __wrap_ph_mqtt_session_run(Client *c,const ph_mqtt_session_hooks *hooks){
 assert(ph_mqtt_owner());session_runs++;
 /* Bound only the test lifetime. Transport, owner binding and session are real. */
 ph_mqtt_session_hooks bounded=*hooks;bounded.stop=stop;
 __real_ph_mqtt_session_run(c,&bounded);
}
#define session_prepare prepare
#define session_subscription subscription_plan
#define session_initial initial
#define session_cycle cycle
#define session_closing closing
#define session_event event
/* WORKER */
/* START */
static void dispatch(void){
 assert(pending_worker);void (*worker)(mico_thread_arg_t)=pending_worker;
 pending_worker=NULL;worker(pending_argument);
}
int main(int argc,char **argv){
 assert(argc==2);unsigned mode=(unsigned)atoi(argv[1]);
 assert(mqtt_account_init(&account,MQTT_TOPIC_TMCP,"m1","fixture","aabbccddeeff"));
 if(mode==3){
  create_fail=true;assert(m1_mqtt_start()==-1&&!pending_worker);
  assert(created==1&&!owner_attempts&&!session_runs&&!deleted);
  create_fail=false;
 }
 owner_failures=mode==1?3:mode==2?30:0;
 unsigned failures=owner_failures;
 assert(m1_mqtt_start()==kNoErr&&pending_worker);
 dispatch();
 assert(created==(mode==3?2u:1u)&&!deleted);
 assert(owner_attempts==failures+1&&session_runs==1&&ready_events==1);
 assert(now_ms>=failures*1000u&&cycles==2&&close_events==1);
 assert(!native_live&&!charged_live);ph_mqtt_owner_end();
 assert(!ph_mqtt_owner());
 printf("M1 actual startup/session: case=%u init_attempts=%u threads=%u READY=%u PASS\n",
        mode,owner_attempts,created,ready_events);
 return 0;
}
'''.replace('/* WORKER */', worker).replace('/* START */', start)

    @classmethod
    def build(cls, source, binary):
        path = binary.with_suffix('.c')
        path.write_text(source)
        protocol = cls.protocol
        subprocess.run([os.environ.get('CC', 'cc'), '-D_GNU_SOURCE', '-std=gnu99', '-g',
            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I' + str(ROOT),
            '-I' + str(ROOT / 'common/mqtt'), '-I' + str(ROOT / 'tests/mqtt_core/stubs'),
            '-I' + str(protocol), '-I' + str(protocol / 'mico'), '-I' + str(protocol / 'MQTTPacket'),
            '-DMAX_MESSAGE_HANDLERS=12', '-DPH_EXPECTED_MESSAGE_HANDLERS=12',
            str(path), str(ROOT / 'common/mqtt/core.c'), str(ROOT / 'common/mqtt/client.c'),
            str(ROOT / 'common/mqtt/session.c'), str(protocol / 'MQTTClient.c'),
            *map(str, sorted((protocol / 'MQTTPacket').glob('*.c'))),
            '-Wl,--wrap=mico_rtos_init_mutex,--wrap=ph_mqtt_session_run',
            '-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup',
            '-o', str(binary)], check=True, timeout=60)

    def case(self, mode):
        subprocess.run([str(self.binary), str(mode)], check=True, timeout=20)

    def test_normal_startup(self): self.case(0)
    def test_transient_mutex_failure_recovers_in_same_worker(self): self.case(1)
    def test_repeated_mutex_failure_does_not_spawn_workers(self): self.case(2)
    def test_thread_creation_failure_allows_later_start(self): self.case(3)

    def test_previous_exit_on_owner_failure_is_a_negative_control(self):
        previous = (ROOT / 'M1/mqtt.c').read_text()
        old_worker = '''static void mqtt_worker(mico_thread_arg_t arg) {
    if(!ph_mqtt_startup_wait(&mqtt_startup,(uint32_t)arg) ||
       !ph_mqtt_owner_init(&account,8192,&mqtt_owner_thread,ingress_accept)) {
        mqtt_status(MQTT_CONFIG_ERROR);mico_rtos_delete_thread(NULL);return;
    }
    const ph_mqtt_session_hooks hooks={NULL,session_prepare,session_subscription,
        session_initial,session_cycle,session_closing,session_event,NULL,session_initial_ready,session_valid};
    ph_mqtt_session_run(&client,&hooks);
}'''
        previous = previous.replace(function(previous, 'static void mqtt_worker('), old_worker)
        binary = self.path / 'previous-startup'
        self.build(self.fixture(previous), binary)
        result = subprocess.run([str(binary), '1'], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=20)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
