"""Production M1 callbacks and shared/Paho sessions over an in-memory broker."""
import os
import re
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_http_json_budget import function
from tests.m1_mqtt_fixture import observe_discovery
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
            'static bool session_subscription(',
            'static __attribute__((noinline)) int subscribe_metadata(', 'static int retry_metadata(',
            'static int session_initial(',
            'static bool session_valid(', 'static void session_closing(',
            'static void session_event('))
        # The mock broker must grant the requested QoS (HA birth requests 0).
        # The production SDK, shared session, and extracted callbacks are intact.
        paho = (ROOT / 'tests/mqtt_core/paho.c').read_text().replace(
            'subscribe_fail?0x80:1', 'subscribe_fail?0x80:tx[tx_size-1]')
        paho = paho.replace('append(ack,sizeof(ack));\n }\n if(tx[0]>>4==3)',
            'if(diagnostic_ack_mode==2)ack[3]^=1;\n'
            '  if(diagnostic_ack_mode!=3)append(ack,sizeof(ack));\n }\n if(tx[0]>>4==3)')
        paho = paho.replace('if(diagnostic_ack_mode==2)ack[3]^=1;', r'''unsigned topic_length=(tx[offset+2]<<8)|tx[offset+3];
  bool metadata=topic_length>=9&&!memcmp(tx+offset+4+topic_length-9,"/metadata",9);
  if(metadata_scenario==11&&topic_length>=11&&!memcmp(tx+offset+4+topic_length-11,"/sensor/+/+",11))ack[4]=128;
  if(metadata){
   metadata_requests++;
   if(metadata_scenario>=19&&metadata_requests==1)early_metadata();
   if(metadata_ack_mode==1)ack[4]=128;
   if(metadata_ack_mode==2)ack[3]^=1;
   if(metadata_ack_mode==3)return;
   if(metadata_ack_mode==4)ack[0]=0x91;
   if(metadata_ack_mode==5){metadata_read_fail=true;return;}
  }
  if(diagnostic_ack_mode==2)ack[3]^=1;''')
        paho = paho.replace('if(rx_at==rx_size){now_ms+=',
                            'if(metadata_read_fail)return -1;\n if(rx_at==rx_size){now_ms+=')
        paho = observe_discovery(paho)
        cls.source = r'''
static unsigned diagnostic_ack_mode,metadata_ack_mode,metadata_requests,metadata_scenario;
static unsigned metadata_read_fail;
static void early_metadata(void);
#define main existing_paho_scenarios
#define publish paho_publish
/* PAHO */
#undef publish
#undef main
#include "M1/display_source.h"
#include "common/typed_wire.h"
#include "common/sensor_mp.h"
#define M1_VERSION "1.0.0-rc.4-test"
#define PHICOMM_FIRMWARE_VARIANT "m1-test"
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
static bool session_need_availability,session_subscribed,rediscover,cleanup_pending,metadata_subscribed;
static uint32_t metadata_subscribe_at;
static bool discovery_deferred,co2_discovered,pressure_discovered,particles_discovered,sensor_metadata,sensor_started;
static unsigned discovery_language,discovery_retry_at,discovery_index,initial_index;
static unsigned connect_sequence,co2_sequence,pressure_sequence,sequence,particles_at,snapshot_at;
static int old_co2_fresh,old_pressure_fresh,old_brightness,old_fresh;
static char base[192],command_topic[512],availability_topic[512],client_id[192],wildcard[512];
static char sensor_filter[512],source_availability_filter[512];
/* METADATA FILTERS */
static char m1_id[13]="aabbccddeeff";
static unsigned inputs,resets,capabilities,initial_calls,session_count,sub_errors,ready_count;
static unsigned plans[4],initial_counts[4],last_subscribe_bytes;
static int final_rc;
static unsigned deny_availability,transition,stop_after=1;
static bool cache_valid,received_before_initial,changed,observed_live,require_metadata,descriptor_valid;
static unsigned metadata_inputs,descriptor_boot,descriptor_revision,native_inputs;
static void m1_config_get(m1_config_t *out){*out=live;}
static void m1_display_config_get(m1_display_config_t *out){*out=display;}
static void mqtt_state_get(mqtt_state_t *out){out->wifi_connected=m1_state.wifi_connected;out->mqtt_status=m1_state.mqtt_status;}
static void m1_display_source_mqtt_reset(void){resets++;cache_valid=descriptor_valid=false;}
static void feature_mqtt_session(const mqtt_account_t *a){(void)a;m1_display_source_mqtt_reset();}
static bool feature_mqtt_input(const char *t,const void *p,size_t n,bool retained){
 bool metadata=strstr(t,"/metadata")!=NULL;ph_value body;ph_sensor_view view;
 assert(phv_parse(PH_WIRE_MSGPACK,p,n,&body)&&ph_sensor_view_parse(&body,"123456abcdef",metadata,&view));
 assert(memchr(p,0,n));
 if(metadata){
  metadata_inputs++;descriptor_boot=view.boot;descriptor_revision=view.descriptor_revision;
  descriptor_valid=ph_sensor_descriptor_matches(&view,"co2","ppm");return true;
 }
 assert(strstr(t,"/sensor/123456abcdef/state")&&!retained);inputs++;
 cache_valid=!require_metadata||(descriptor_valid&&descriptor_boot==view.boot&&descriptor_revision==view.descriptor_revision);return true;
}
static void feature_mqtt_service(int (*p)(const char*,const void*,size_t,bool),bool connected){(void)p;(void)connected;}
static int publish(const char *t,const char *p,bool r){(void)t;(void)p;(void)r;return 0;}
static int publish_leaf(const char *t,const char *p,bool r){return publish(t,p,r);}
static int publish_bytes(const char *t,const void *p,size_t n,bool r){assert(n);(void)t;(void)p;(void)r;return 0;}
static int service_mqtt_capabilities(int (*p)(const char*,const void*,size_t,bool),const mqtt_account_t *a){(void)p;(void)a;capabilities++;return 0;}
static void service_mqtt_input(int (*p)(const char*,const void*,size_t,bool),const mqtt_account_t *a,const void *d,size_t n,bool r){(void)p;(void)a;ph_value v;if(!r&&phv_parse(PH_WIRE_MSGPACK,d,n,&v))native_inputs++;}
static void service_mqtt_clear(void){}
static void ota_job_poll(void){}
static bool ota_job_active(void){return false;}
static void ota_command(const char *b,const void *p,size_t n,bool r){(void)b;(void)p;(void)n;(void)r;}
static void ota_forget(void){}
static void ha_command_input(const void *p,size_t n){(void)p;(void)n;assert(0);}
static unsigned device_language_get(void){return 0;}
static int cleanup_all(void){return 0;}
static int mqtt_cleanup_reconnect(Client *c,mqtt_account_t *a,const char *h,unsigned p,const char *u,const char *s,int (*f)(void)){
 (void)c;(void)a;(void)h;(void)p;(void)u;(void)s;(void)f;return 0;
}
int m1_mqtt_read(Network *n,unsigned char *p,int bytes,int timeout){return read_packet(n,p,bytes,timeout);}
static bool same_connection(const m1_config_t*,const m1_config_t*);
/* CALLBACKS */
static void receive(void){
 char topic_bytes[512];assert(mqtt_aux(&account,"sensor","123456abcdef/state",topic_bytes,sizeof(topic_bytes)));
 MQTTString topic=MQTTString_initializer;topic.cstring=topic_bytes;
 unsigned char bytes[256];ph_writer writer;ph_buf buffer;
 assert(phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,bytes,sizeof(bytes)));
 phw_begin(&writer,NULL,false);phw_int(&writer,"schema",3);phw_text(&writer,"device_id","123456abcdef");
 phw_u32(&writer,"boot",1);phw_u32(&writer,"sequence",1);phw_u32(&writer,"descriptor_revision",1);
 phw_begin(&writer,"sensors",false);phw_end(&writer);phw_end(&writer);size_t length=phw_finish_fixed(&writer);assert(length);
 MQTTMessage msg={0};msg.qos=QOS1;msg.payload=bytes;msg.payloadlen=length;
 MessageData data={&msg,&topic};native_command(&data);
}
static void configure(bool mqtt){
 display.configured=1;display.revision++;
 display.lanes[0].source=mqtt?M1_DISPLAY_MQTT:M1_DISPLAY_API;
 m1_display_source_mqtt_reset();
}
static int prepare_product(void *ctx,ph_mqtt_connection *spec){
 assert(session_count<4);rx_at=rx_size=tx_size=0;subscribe_fail=false;diagnostic_ack_mode=0;metadata_read_fail=false;
 plans[session_count]=0;session_count++;
 return session_prepare(ctx,spec);
}
static bool subscribe_product(void *ctx,unsigned index,ph_mqtt_subscription *s){
 bool present=session_subscription(ctx,index,s);
 if(!present){subscribe_fail=false;diagnostic_ack_mode=0;}
 if(present){
  plans[session_count-1]++;
  if(index==5){last_subscribe_bytes=(unsigned)strlen(s->filter)+7;assert(s->handler==native_command&&s->qos==QOS1);}
  subscribe_fail=index==5&&deny_availability==1;
  diagnostic_ack_mode=index==5?deny_availability:0;
  /* Simulate an application delivery before the plan is fully acknowledged. */
  unsigned before=inputs;receive();assert(inputs==before&&!cache_valid);
 }
 return present;
}
static int initial_product(void *ctx){
 initial_calls++;initial_counts[session_count-1]++;
 if(transition==3&&!changed){changed=true;configure(true);}
 int rc=session_initial(ctx);
 assert(!client.defaultMessageHandler&&!metadata_inputs);
 if(rc==0){assert(session_subscribed);receive();assert(cache_valid==!require_metadata);observed_live=true;}
 return rc;
}
static void wire(const char *topic,const void *bytes,size_t length,bool retained){
 unsigned char packet[1536];MQTTString name=MQTTString_initializer;name.cstring=(char *)topic;
 int n=MQTTSerialize_publish(packet,sizeof(packet),0,1,retained,701,name,(unsigned char *)bytes,length);assert(n>0);
 unsigned before=inputs+metadata_inputs+native_inputs;append(packet,n);
 assert(!ph_mqtt_yield(&client,1));assert(inputs+metadata_inputs+native_inputs==before);
 memset(packet,0,(size_t)n);assert(!ph_mqtt_poll(&client));
}
static void sensor_wire(bool metadata,bool retained,bool foreign,unsigned boot,unsigned revision){
 unsigned char bytes[512];ph_writer w;ph_buf buffer;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,bytes,sizeof(bytes)));
 if(metadata){ph_sensor_metadata_begin(&w,"123456abcdef",boot,revision);ph_sensor_descriptor_write(&w,"co2","CO2","ppm");}
 else{ph_sensor_snapshot_begin(&w,"123456abcdef",boot,10,revision);ph_sensor_reading_write(&w,"co2",600,true,0);}
 ph_sensor_finish(&w);size_t n=phw_finish_fixed(&w);assert(n);
 mqtt_account_t target=account;if(foreign)strcpy(target.key,"u_other");char topic[512];
 assert(mqtt_aux(&target,"sensor",metadata?"123456abcdef/metadata":"123456abcdef/state",topic,sizeof(topic)));
 wire(topic,bytes,n,retained);memset(bytes,0,n);
}
static void early_metadata(void){
 unsigned char bytes[512],packet[1024];ph_writer w;ph_buf buffer;
 assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,bytes,sizeof(bytes)));
 ph_sensor_metadata_begin(&w,"123456abcdef",1,1);ph_sensor_descriptor_write(&w,"co2","CO2","ppm");
 ph_sensor_finish(&w);size_t length=phw_finish_fixed(&w);assert(length);
 char topic[512];assert(mqtt_aux(&account,"sensor","123456abcdef/metadata",topic,sizeof(topic)));
 MQTTString name=MQTTString_initializer;name.cstring=topic;
 int n=MQTTSerialize_publish(packet,sizeof(packet),0,1,1,702,name,bytes,length);assert(n>0);
 append(packet,n);memset(bytes,0,sizeof(bytes));memset(packet,0,sizeof(packet));
}
static void native_wire(bool retained,bool json){
 unsigned char bytes[128];ph_writer w;ph_buf buffer;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,bytes,sizeof(bytes)));
 phw_begin(&w,NULL,false);phw_text(&w,"action","status.get");phw_end(&w);size_t n=phw_finish_fixed(&w);assert(n);
 const char text[]="{\"action\":\"status.get\"}";
 wire(wildcard,json?(const void *)text:bytes,json?sizeof(text)-1:n,retained);
}
static int cycle_product(void *ctx){
 (void)ctx;cycles++;
 if(metadata_scenario>=11){
  unsigned before=metadata_requests;
  assert(session_subscribed&&m1_state.mqtt_status==MQTT_READY);
  native_wire(false,false);assert(native_inputs==cycles);
  native_wire(false,true);native_wire(true,false);assert(native_inputs==cycles);
  if(metadata_scenario==11){
   assert(!metadata_subscribed&&!cache_valid);sensor_wire(true,true,false,1,1);
   assert(!metadata_inputs&&!cache_valid);receive();assert(!cache_valid);
  }
  if(metadata_scenario==12){
   assert(metadata_subscribed);sensor_wire(false,false,false,1,1);assert(!cache_valid);
   sensor_wire(true,true,true,1,1);assert(!metadata_inputs);
   sensor_wire(true,true,false,1,1);assert(metadata_inputs==cycles&&descriptor_valid&&!cache_valid);
   sensor_wire(false,false,false,1,1);assert(cache_valid);
   unsigned samples=inputs;sensor_wire(false,true,false,1,1);assert(inputs==samples&&cache_valid);
   sensor_wire(false,false,false,2,2);assert(!cache_valid);
  }
  if(metadata_scenario==19){
   assert(metadata_subscribed&&metadata_inputs==1&&descriptor_valid);
   sensor_wire(false,false,false,1,1);assert(cache_valid);
  }
  if(metadata_scenario==20){
   assert(!metadata_subscribed&&!metadata_inputs&&!descriptor_valid);
   now_ms=metadata_subscribe_at+30000;metadata_ack_mode=0;countdown(&client.ping_timer,30);
   assert(!retry_metadata()&&metadata_subscribed&&metadata_requests==2);
   assert(!ph_mqtt_poll(&client)&&!metadata_inputs&&!descriptor_valid);
   sensor_wire(false,false,false,1,1);assert(!cache_valid);
   sensor_wire(true,true,false,2,2);assert(metadata_inputs==1);
   sensor_wire(false,false,false,1,1);assert(!cache_valid);
   sensor_wire(false,false,false,2,2);assert(cache_valid);before=metadata_requests;
  }
  if(metadata_scenario==17||metadata_scenario==18){
   assert(!metadata_subscribed&&!cache_valid);metadata_subscribe_at=UINT32_MAX-14999;now_ms=metadata_subscribe_at;
   for(unsigned i=0;i<30000;i++){assert(!retry_metadata()&&metadata_requests==before);now_ms++;}
   metadata_ack_mode=metadata_scenario==17?0:3;countdown(&client.ping_timer,30);
   int rc=retry_metadata();
   if(metadata_scenario==18){assert(rc<0&&rc!=MQTT_SUBSCRIBE_DENIED);return rc;}
   assert(!rc&&metadata_subscribed&&metadata_requests==before+1);
   sensor_wire(true,true,false,1,1);sensor_wire(false,false,false,1,1);assert(cache_valid);
   before=metadata_requests;
  }
  if(metadata_subscribed){now_ms=metadata_subscribe_at+90000;assert(!retry_metadata()&&metadata_requests==before);countdown(&client.ping_timer,30);}
  else assert(metadata_requests==before);
  assert(m1_state.mqtt_status==MQTT_READY);return 0;
 }
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
static bool stop_product(void *ctx){(void)ctx;return close_events>=stop_after||cycles>=(metadata_scenario>=11?1u:stop_after+1);}
static void run(void){
 ph_mqtt_session_hooks hooks={NULL,prepare_product,subscribe_product,initial_product,
   cycle_product,closing_product,event_product,stop_product,NULL,session_valid};
 assert(ph_mqtt_owner_init(&account,8192,&thread,ingress_accept));
 ph_mqtt_session_run(&client,&hooks);ph_mqtt_owner_end();
 assert(!native_live&&!charged_live&&!session_subscribed&&!cache_valid);
}
int main(int argc,char **argv){
 assert(argc==2);unsigned mode=(unsigned)atoi(argv[1]);
 strcpy(live.mqtt_host,"fixture");strcpy(live.mqtt_user,"abc");
 assert(sizeof(co2_filter)+sizeof(metadata_filter)<512);
 if(mode==12){memset(live.mqtt_user,'x',MQTT_USER_MAX);live.mqtt_user[MQTT_USER_MAX]=0;}
 strcpy(live.name,"fixture");live.mqtt_port=1883;m1_state.wifi_connected=true;
 m1_display_defaults(&display);
 if(mode==0){display.lanes[0].source=M1_DISPLAY_MQTT;deny_availability=1;run();assert(plans[0]==5&&ready_count==1&&!sub_errors);}
 if(mode==1){configure(false);display.lanes[1].source=M1_DISPLAY_LOCAL;deny_availability=1;run();assert(plans[0]==5&&ready_count==1&&!sub_errors);}
 if(mode==2){configure(true);display.selected=0;deny_availability=1;run();assert(plans[0]==6&&sub_errors==1&&!initial_calls&&!ready_count&&!inputs&&final_rc==MQTT_SUBSCRIBE_DENIED&&last_subscribe_bytes==43);}
 if(mode==3){configure(true);display.selected=1;run();assert(plans[0]==6&&ready_count==1&&!sub_errors&&observed_live);}
 if(mode==4){configure(false);transition=1;stop_after=2;run();assert(session_count==2&&plans[0]==5&&plans[1]==6&&ready_count==2&&initial_calls==2);}
 if(mode==5){configure(true);transition=2;stop_after=2;run();assert(session_count==2&&plans[0]==6&&plans[1]==5&&ready_count==2&&initial_calls==2);}
 if(mode==6){configure(true);transition=4;run();assert(session_count==1&&plans[0]==6&&ready_count==1&&changed);}
 if(mode==7){configure(false);transition=3;stop_after=2;run();assert(session_count==2&&plans[0]==5&&plans[1]==6&&ready_count==1&&initial_calls==2&&observed_live);}
 if(mode==8||mode==9){configure(true);deny_availability=mode==8?2:3;run();assert(plans[0]==6&&sub_errors==1&&!initial_calls&&!ready_count&&!inputs&&final_rc==MQTT_FAILURE);}
 if(mode==10){
  configure(true);ph_mqtt_connection spec;assert(session_prepare(NULL,&spec)==PH_MQTT_PREPARE_READY);
  unsigned before=inputs;receive();assert(inputs==before&&!cache_valid);
  assert(ph_mqtt_owner_init(&account,8192,&thread,ingress_accept));init();
  assert(!session_initial(NULL));receive();assert(cache_valid);
  session_event(NULL,PH_MQTT_SUBSCRIBE_ERROR,-1);assert(!cache_valid&&!session_subscribed);
  before=inputs;receive();assert(inputs==before);
  assert(!session_initial(NULL));receive();assert(cache_valid);
  m1_state.wifi_connected=false;assert(!session_valid(NULL)&&!cache_valid&&!session_subscribed);
  assert(session_prepare(NULL,&spec)==PH_MQTT_PREPARE_WAIT&&!cache_valid&&!session_subscribed);
  cleanup();ph_mqtt_owner_end();
 }
 if(mode>=11&&mode<=21){
  metadata_scenario=mode;require_metadata=true;configure(false);
  metadata_ack_mode=mode==11||mode>=17?1:mode==12?0:mode-11;
  if(mode==19)metadata_ack_mode=0;
  if(mode==21)metadata_ack_mode=3;
  if(mode==17)now_ms=UINT32_MAX-14999;
  run();
  if(mode==11||mode==12||(mode>=17&&mode<=20)){assert(ready_count==1&&!sub_errors&&initial_calls==1&&session_count==1&&native_inputs==1);}
  if((mode>=13&&mode<=16)||mode==21){assert(!ready_count&&!sub_errors&&initial_calls==1&&!capabilities&&final_rc<0&&final_rc!=MQTT_SUBSCRIBE_DENIED);}
  if(mode==11){assert(metadata_requests==1&&m1_state.mqtt_errors==1&&strstr(m1_state.mqtt_last_error,"optional metadata"));}
  if(mode==17)assert(metadata_requests==2&&metadata_inputs==1);
  if(mode==18)assert(metadata_requests==2&&final_rc<0&&final_rc!=MQTT_SUBSCRIBE_DENIED);
 }
 if(mode>21)assert(0);
 if(mode!=10)assert(fixture_discoveries>=ready_count);
 printf("M1 display session case %u PASS filters=%u ready=%u subscribe_errors=%u last_packet=%u\n",mode,plans[0],ready_count,sub_errors,last_subscribe_bytes);
 return 0;
}
'''.replace('/* PAHO */', paho).replace('/* CALLBACKS */', callbacks)
        filters = re.search(r'static char co2_filter\[.*?;', product, re.S).group(0)
        cls.source = cls.source.replace('/* METADATA FILTERS */', filters)
        cls.binary = cls.path / 'display-session'
        cls.build(cls.source, cls.binary)

    @classmethod
    def build(cls, source, binary, client_source=None):
        path = binary.with_suffix('.c')
        path.write_text(source)
        protocol = cls.protocol
        client = ROOT / 'common/mqtt/client.c'
        if client_source is not None:
            client = binary.with_suffix('.client.c')
            client.write_text(client_source)
        subprocess.run([os.environ.get('CC', 'cc'), '-D_GNU_SOURCE', '-std=gnu99', '-g',
            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I' + str(ROOT),
            '-I' + str(ROOT / 'common/mqtt'), '-I' + str(ROOT / 'tests/mqtt_core/stubs'),
            '-I' + str(protocol), '-I' + str(protocol / 'mico'), '-I' + str(protocol / 'MQTTPacket'),
            '-DMAX_MESSAGE_HANDLERS=12', '-DPH_EXPECTED_MESSAGE_HANDLERS=12',
            str(path), str(ROOT / 'common/mqtt/core.c'), str(client),
            str(ROOT / 'common/mqtt/session.c'), str(ROOT / 'common/typed_wire.c'),
            str(ROOT / 'common/wire_buffer.c'), str(protocol / 'MQTTClient.c'),
            *map(str, sorted((protocol / 'MQTTPacket').glob('*.c'))),
            '-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup',
            '-o', str(binary)], check=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.protocol_context.__exit__(None, None, None)
        cls.temp.cleanup()

    def case(self, mode):
        subprocess.run([str(self.binary), str(mode)], check=True, timeout=20)

    def test_unconfigured_profile_preserves_five_filter_plan(self): self.case(0)
    def test_local_and_api_sources_preserve_five_filter_plan(self): self.case(1)
    def test_original_selection_still_requires_mqtt_availability(self): self.case(2)
    def test_all_six_subacks_enable_user_mqtt_input(self): self.case(3)
    def test_adding_mqtt_source_reconnects_and_clears_cache(self): self.case(4)
    def test_removing_mqtt_source_reconnects_and_clears_cache(self): self.case(5)
    def test_selection_change_keeps_needed_subscription(self): self.case(6)
    def test_configuration_change_during_bootstrap_cannot_open_gate(self): self.case(7)
    def test_wrong_suback_id_is_not_ignored(self): self.case(8)
    def test_missing_suback_is_not_ignored(self): self.case(9)
    def test_prepare_subscribe_failure_and_wifi_loss_close_input_gate(self): self.case(10)

    def test_metadata_denial_preserves_core_ready_and_native_commands(self): self.case(11)
    def test_retained_metadata_uses_real_ingress_with_max_username(self): self.case(12)
    def test_optional_wrong_packet_id_remains_fatal(self): self.case(13)
    def test_optional_timeout_remains_fatal(self): self.case(14)
    def test_optional_malformed_header_remains_fatal(self): self.case(15)
    def test_optional_read_failure_remains_fatal(self): self.case(16)
    def test_denied_metadata_retries_after_30s_across_wrap_then_stops(self): self.case(17)
    def test_retry_transport_failure_ends_the_session(self): self.case(18)
    def test_retained_descriptor_before_suback_waits_for_grant(self): self.case(19)
    def test_denied_early_descriptor_is_not_reused_by_later_grant(self): self.case(20)
    def test_early_descriptor_cannot_mask_suback_timeout(self): self.case(21)

    def test_old_post_suback_route_is_a_failing_negative_control(self):
        client = (ROOT / 'common/mqtt/client.c').read_text()
        needle = 'c->defaultMessageHandler = ingress;'
        self.assertIn(needle, client)
        binary = self.path / 'old-post-suback-route'
        self.build(self.source, binary, client.replace(needle, 'c->defaultMessageHandler = previous;'))
        result = subprocess.run([str(binary), '19'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        self.assertNotEqual(result.returncode, 0)

    def test_broad_sensor_filter_is_a_failing_negative_control(self):
        needle = '"sensor","+/state",co2_filter'
        self.assertIn(needle, self.source)
        binary = self.path / 'old-broad-sensor'
        self.build(self.source.replace(needle, '"sensor","+/+",co2_filter'), binary)
        result = subprocess.run([str(binary), '11'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        self.assertNotEqual(result.returncode, 0)

    def test_old_retained_gate_is_a_failing_negative_control(self):
        needle = 'return handler==native_command && ph_mqtt_topic_matches(metadata_filter,topic,length);'
        self.assertIn(needle, self.source)
        binary = self.path / 'old-retained-gate'
        self.build(self.source.replace(needle, 'return false;'), binary)
        result = subprocess.run([str(binary), '12'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        self.assertNotEqual(result.returncode, 0)

    def test_old_unconditional_filter_is_a_failing_negative_control(self):
        needle = 'index==5 && session_need_availability'
        self.assertIn(needle, self.source)
        binary = self.path / 'old-unconditional'
        self.build(self.source.replace(needle, 'index==5'), binary)
        result = subprocess.run([str(binary), '0'], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=20)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
