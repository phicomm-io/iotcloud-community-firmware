"""Production M1 callbacks and shared/Paho sessions over an in-memory broker."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_http_json_budget import function
from tests import test_shared_mqtt as shared_mqtt

ROOT = Path(__file__).resolve().parents[1]


class DisplayMqttSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.temp.name)
        cls.protocol_context = shared_mqtt.SharedMQTT().protocol('M1')
        cls.protocol = cls.protocol_context.__enter__()
        product = (ROOT / 'M1/mqtt.c').read_text()
        callbacks = '\n'.join(function(product, name) for name in (
            'static bool display_mqtt_needed(', 'static void session_display_reset(',
            'static bool session_current(', 'static void mqtt_status(',
            'static void mqtt_error(', 'static void native_command(',
            'static void ha_birth(', 'static bool same_connection(const m1_config_t *a,',
            'static bool ingress_accept(', 'static int session_prepare(',
            'static bool session_subscription(', 'static int session_initial(',
            'static bool session_valid(', 'static void session_closing(',
            'static void session_event('))
        # The mock broker must grant the requested QoS (HA birth requests 0).
        # The production SDK, shared session, and extracted callbacks are intact.
        paho = (ROOT / 'tests/mqtt_core/paho.c').read_text().replace(
            'subscribe_fail?0x80:1', 'subscribe_fail?0x80:tx[tx_size-1]')
        paho = paho.replace('append(ack,sizeof(ack));\n }\n if(tx[0]>>4==3)',
            'if(diagnostic_ack_mode==2)ack[3]^=1;\n'
            '  if(diagnostic_ack_mode!=3)append(ack,sizeof(ack));\n }\n if(tx[0]>>4==3)')
        cls.source = r'''
static unsigned diagnostic_ack_mode;
#define main existing_paho_scenarios
#define publish paho_publish
/* PAHO */
#undef publish
#undef main
#include "M1/display_source.h"
#define FEATURE_TOPIC_MAX 512
typedef struct {
 char mqtt_host[128],mqtt_user[64],mqtt_password[128],name[64];
 unsigned mqtt_port;
} m1_config_t;
typedef struct {
 struct {unsigned sequence;} sample;
 struct {bool received;} scd40,barometer;
 bool wifi_connected;unsigned brightness_requested,mqtt_status;
} mqtt_state_t;
static m1_config_t live,cfg,current,session_config;
static m1_display_config_t display;
static mqtt_state_t state;
static struct {bool wifi_connected,mqtt_connected,mcu_sensor_details,mqtt_cleanup_warning;
 unsigned mqtt_status,mqtt_errors;char mqtt_last_error[96];} m1_state;
static unsigned m1_lock;
static bool session_need_availability,session_subscribed,rediscover,cleanup_pending;
static bool discovery_deferred,co2_discovered,pressure_discovered,particles_discovered;
static unsigned discovery_language,discovery_retry_at,discovery_index,initial_index;
static unsigned connect_sequence,co2_sequence,pressure_sequence,sequence,particles_at,snapshot_at;
static int old_co2_fresh,old_pressure_fresh,old_brightness,old_fresh;
static char base[192],command_topic[512],availability_topic[512],client_id[192],wildcard[512];
static char sensor_filter[512],co2_filter[512],source_availability_filter[512];
static char m1_id[13]="aabbccddeeff";
static unsigned inputs,resets,capabilities,initial_calls,session_count,sub_errors,ready_count;
static unsigned plans[4],initial_counts[4],last_subscribe_bytes;
static int final_rc;
static unsigned deny_availability,transition,stop_after=1;
static bool cache_valid,received_before_initial,changed,observed_live;
static void m1_config_get(m1_config_t *out){*out=live;}
static void m1_display_config_get(m1_display_config_t *out){*out=display;}
static void mqtt_state_get(mqtt_state_t *out){out->wifi_connected=m1_state.wifi_connected;out->mqtt_status=m1_state.mqtt_status;}
static void m1_display_source_mqtt_reset(void){resets++;cache_valid=false;}
static void feature_mqtt_session(const mqtt_account_t *a){(void)a;m1_display_source_mqtt_reset();}
static bool feature_mqtt_input(const char *t,const void *p,size_t n,bool retained){
 (void)t;(void)p;(void)n;assert(!retained);inputs++;cache_valid=true;return true;
}
static void feature_mqtt_service(int (*p)(const char*,const char*,bool),bool connected){(void)p;(void)connected;}
static int publish(const char *t,const char *p,bool r){(void)t;(void)p;(void)r;return 0;}
static int publish_leaf(const char *t,const char *p,bool r){return publish(t,p,r);}
static int service_mqtt_capabilities(int (*p)(const char*,const char*,bool),const mqtt_account_t *a){(void)p;(void)a;capabilities++;return 0;}
static void service_mqtt_input(int (*p)(const char*,const char*,bool),const mqtt_account_t *a,const void *d,size_t n,bool r){(void)p;(void)a;(void)d;(void)n;(void)r;}
static void service_mqtt_clear(void){}
static void ota_job_poll(void){}
static bool ota_job_active(void){return false;}
static void ota_command(const char *b,const void *p,size_t n,bool r){(void)b;(void)p;(void)n;(void)r;}
static void ota_forget(void){}
static void brightness_command(MessageData *d){(void)d;assert(0);}
static unsigned device_language_get(void){return 0;}
static int cleanup_all(void){return 0;}
static int mqtt_cleanup_reconnect(Client *c,mqtt_account_t *a,const char *h,unsigned p,const char *u,const char *s,int (*f)(void)){
 (void)c;(void)a;(void)h;(void)p;(void)u;(void)s;(void)f;return 0;
}
int m1_mqtt_read(Network *n,unsigned char *p,int bytes,int timeout){return read_packet(n,p,bytes,timeout);}
static bool same_connection(const m1_config_t*,const m1_config_t*);
/* CALLBACKS */
static void receive(void){
 MQTTString topic=MQTTString_initializer;topic.cstring=co2_filter;
 MQTTMessage msg={0};msg.qos=QOS1;msg.payload="{}";msg.payloadlen=2;
 MessageData data={&msg,&topic};native_command(&data);
}
static void configure(bool mqtt){
 display.configured=1;display.revision++;
 display.lanes[0].source=mqtt?M1_DISPLAY_MQTT:M1_DISPLAY_API;
 m1_display_source_mqtt_reset();
}
static int prepare_product(void *ctx,ph_mqtt_connection *spec){
 assert(session_count<4);rx_at=rx_size=tx_size=0;subscribe_fail=false;diagnostic_ack_mode=0;
 plans[session_count]=0;session_count++;
 return session_prepare(ctx,spec);
}
static bool subscribe_product(void *ctx,unsigned index,ph_mqtt_subscription *s){
 bool present=session_subscription(ctx,index,s);
 if(present){
  plans[session_count-1]++;
  if(index==4){last_subscribe_bytes=(unsigned)strlen(s->filter)+7;assert(s->handler==native_command&&s->qos==QOS1);}
  subscribe_fail=index==4&&deny_availability==1;
  diagnostic_ack_mode=index==4?deny_availability:0;
  /* Simulate an application delivery before the plan is fully acknowledged. */
  unsigned before=inputs;receive();assert(inputs==before&&!cache_valid);
 }
 return present;
}
static int initial_product(void *ctx){
 initial_calls++;initial_counts[session_count-1]++;
 if(transition==3&&!changed){changed=true;configure(true);}
 int rc=session_initial(ctx);
 if(rc==0){assert(session_subscribed);receive();assert(cache_valid);observed_live=true;}
 return rc;
}
static int cycle_product(void *ctx){
 (void)ctx;cycles++;
 if((transition==1||transition==2)&&!changed){
  changed=true;configure(transition==1);
  unsigned before=inputs;receive();assert(!cache_valid&&inputs==before&&!session_subscribed);
  assert(!session_valid(NULL));
 }else if(transition==4&&!changed){
  changed=true;display.selected=!display.selected;display.revision++;
  assert(session_valid(NULL)&&session_subscribed);receive();assert(cache_valid);
 }
 return 0;
}
static void closing_product(void *ctx,bool connected,int rc){
 final_rc=rc;close_events++;session_closing(ctx,connected,rc);
 assert(!cache_valid&&!session_subscribed);unsigned before=inputs;receive();assert(inputs==before);
}
static void event_product(void *ctx,unsigned e,int rc){
 session_event(ctx,e,rc);
 if(e==PH_MQTT_SUBSCRIBE_ERROR){sub_errors++;assert(m1_state.mqtt_status==MQTT_WAITING_ACL&&!cache_valid&&!session_subscribed);}
 if(e==PH_MQTT_READY)ready_count++;
}
static bool stop_product(void *ctx){(void)ctx;return close_events>=stop_after||cycles>=stop_after+1;}
static void run(void){
 ph_mqtt_session_hooks hooks={NULL,prepare_product,subscribe_product,initial_product,
   cycle_product,closing_product,event_product,stop_product,NULL,session_valid};
 assert(ph_mqtt_owner_init(&account,8192,&thread,ingress_accept));
 ph_mqtt_session_run(&client,&hooks);ph_mqtt_owner_end();
 assert(!native_live&&!charged_live&&!session_subscribed&&!cache_valid);
}
int main(int argc,char **argv){
 assert(argc==2);strcpy(live.mqtt_host,"fixture");strcpy(live.mqtt_user,"abc");
 strcpy(live.name,"fixture");live.mqtt_port=1883;m1_state.wifi_connected=true;
 m1_display_defaults(&display);unsigned mode=(unsigned)atoi(argv[1]);
 if(mode==0){display.lanes[0].source=M1_DISPLAY_MQTT;deny_availability=1;run();assert(plans[0]==4&&ready_count==1&&!sub_errors);}
 if(mode==1){configure(false);display.lanes[1].source=M1_DISPLAY_LOCAL;deny_availability=1;run();assert(plans[0]==4&&ready_count==1&&!sub_errors);}
 if(mode==2){configure(true);display.selected=0;deny_availability=1;run();assert(plans[0]==5&&sub_errors==1&&!initial_calls&&!ready_count&&!inputs&&final_rc==MQTT_FAILURE&&last_subscribe_bytes==43);}
 if(mode==3){configure(true);display.selected=1;run();assert(plans[0]==5&&ready_count==1&&!sub_errors&&observed_live);}
 if(mode==4){configure(false);transition=1;stop_after=2;run();assert(session_count==2&&plans[0]==4&&plans[1]==5&&ready_count==2&&initial_calls==2);}
 if(mode==5){configure(true);transition=2;stop_after=2;run();assert(session_count==2&&plans[0]==5&&plans[1]==4&&ready_count==2&&initial_calls==2);}
 if(mode==6){configure(true);transition=4;run();assert(session_count==1&&plans[0]==5&&ready_count==1&&changed);}
 if(mode==7){configure(false);transition=3;stop_after=2;run();assert(session_count==2&&plans[0]==4&&plans[1]==5&&ready_count==1&&initial_calls==2&&observed_live);}
 if(mode==8||mode==9){configure(true);deny_availability=mode==8?2:3;run();assert(plans[0]==5&&sub_errors==1&&!initial_calls&&!ready_count&&!inputs&&final_rc==MQTT_FAILURE);}
 if(mode==10){
  configure(true);ph_mqtt_connection spec;assert(session_prepare(NULL,&spec)==PH_MQTT_PREPARE_READY);
  unsigned before=inputs;receive();assert(inputs==before&&!cache_valid);
  assert(!session_initial(NULL));receive();assert(cache_valid);
  session_event(NULL,PH_MQTT_SUBSCRIBE_ERROR,-1);assert(!cache_valid&&!session_subscribed);
  before=inputs;receive();assert(inputs==before);
  assert(!session_initial(NULL));receive();assert(cache_valid);
  m1_state.wifi_connected=false;assert(!session_valid(NULL)&&!cache_valid&&!session_subscribed);
  assert(session_prepare(NULL,&spec)==PH_MQTT_PREPARE_WAIT&&!cache_valid&&!session_subscribed);
 }
 if(mode>10)assert(0);
 printf("M1 display session case %u PASS filters=%u ready=%u subscribe_errors=%u last_packet=%u\n",mode,plans[0],ready_count,sub_errors,last_subscribe_bytes);
 return 0;
}
'''.replace('/* PAHO */', paho).replace('/* CALLBACKS */', callbacks)
        cls.binary = cls.path / 'display-session'
        cls.build(cls.source, cls.binary)

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
            '-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup',
            '-o', str(binary)], check=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.protocol_context.__exit__(None, None, None)
        cls.temp.cleanup()

    def case(self, mode):
        subprocess.run([str(self.binary), str(mode)], check=True, timeout=20)

    def test_unconfigured_profile_preserves_four_filter_plan(self): self.case(0)
    def test_local_and_api_sources_preserve_four_filter_plan(self): self.case(1)
    def test_original_selection_still_requires_mqtt_availability(self): self.case(2)
    def test_all_five_subacks_enable_user_mqtt_input(self): self.case(3)
    def test_adding_mqtt_source_reconnects_and_clears_cache(self): self.case(4)
    def test_removing_mqtt_source_reconnects_and_clears_cache(self): self.case(5)
    def test_selection_change_keeps_needed_subscription(self): self.case(6)
    def test_configuration_change_during_bootstrap_cannot_open_gate(self): self.case(7)
    def test_wrong_suback_id_is_not_ignored(self): self.case(8)
    def test_missing_suback_is_not_ignored(self): self.case(9)
    def test_prepare_subscribe_failure_and_wifi_loss_close_input_gate(self): self.case(10)

    def test_old_unconditional_filter_is_a_failing_negative_control(self):
        needle = 'index==4 && session_need_availability'
        self.assertIn(needle, self.source)
        binary = self.path / 'old-unconditional'
        self.build(self.source.replace(needle, 'index==4'), binary)
        result = subprocess.run([str(binary), '0'], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=20)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
