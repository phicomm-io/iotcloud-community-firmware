"""Actual shared queue and pinned Paho protocol; transport is in-memory, no sockets."""
import os
import io
import tarfile
from contextlib import contextmanager
from tools.apply_sdk_patch import apply_stack
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.test_http_json_owned import copy_patched_json
ROOT = Path(__file__).resolve().parents[1]
class SharedMQTT(unittest.TestCase):
    @contextmanager
    def protocol(self, model):
        name = 'tc1-sdk-3.6.4' if model == 'TC1' else model.lower() + '-sdk'
        sdk = Path(os.environ.get('PHICOMM_' + model + '_SDK', ROOT / 'build' / name / 'mico-os'))
        self.assertTrue((sdk / 'libraries/protocols/mqtt/MQTTClient.c').is_file(),
                        model + ' pinned SDK required; run tools/prepare_sdk.py')
        pin = subprocess.check_output(['python3', str(ROOT/'tools/prepare_sdk.py'), '--commit'], text=True).strip()
        actual = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip()
        self.assertEqual(actual, pin, 'protocol fixture must use the production SDK pin')
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory)
            archive = subprocess.check_output(['git', '-C', str(sdk), 'archive', 'HEAD', 'libraries/protocols/mqtt'])
            with tarfile.open(fileobj=io.BytesIO(archive)) as source:
                source.extractall(copy, filter='data')
            if model != 'TC1':
                # Preserve each product's real Yield semantics. Other board files
                # in sdk.patch are deliberately outside this isolated fixture.
                parts = (ROOT/model/'sdk.patch').read_bytes().split(b'diff --git ')
                patch = copy/'product.patch'
                patch.write_bytes(b''.join(b'diff --git '+part for part in parts[1:]
                    if part.startswith(b'a/libraries/protocols/mqtt/')))
                if patch.stat().st_size:
                    apply_stack(copy, [patch])
            if model != 'TC1':
                apply_stack(copy, [ROOT/model/'features-sdk.patch'])
            apply_stack(copy, [ROOT/p for p in ('common/mqtt-mico36.patch',
                'common/mqtt-publish-bounds.patch', 'common/mqtt-socket-close.patch',
                'M1/mqtt-packet-deadline.patch', 'common/mqtt-response-deadline.patch',
                'common/mqtt-buffer-storage.patch')])
            yield copy/'libraries/protocols/mqtt'

    def compile_run(self, sources, includes=(), defines=()):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'test'
            command = [os.environ.get('CC', 'cc'), '-D_GNU_SOURCE', '-std=gnu99', '-g',
                       '-fsanitize=' + os.environ.get('PHICOMM_MQTT_SANITIZER','address,undefined'), '-fno-pie', '-no-pie',
                       '-I' + str(ROOT / 'common/mqtt')]
            command += ['-I' + str(p) for p in includes]
            command += list(defines) + [str(p) for p in sources] + ['-o', str(binary)]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=30)
    def test_core(self):
        self.compile_run([ROOT / p for p in ('tests/mqtt_core/core.c', 'common/mqtt/core.c')])
    def test_concurrent_clear(self):
        self.compile_run([ROOT/"tests/mqtt_core/concurrent.c", ROOT/"common/mqtt/core.c"], defines=["-pthread"])
    def check_protocol(self, model):
        with self.protocol(model) as protocol:
            self.compile_run([ROOT / 'tests/mqtt_core/paho.c', ROOT / 'common/mqtt/core.c',
                              ROOT / 'common/mqtt/client.c', ROOT / 'common/mqtt/session.c', protocol / 'MQTTClient.c',
                              *sorted((protocol / 'MQTTPacket').glob('*.c'))],
                             [ROOT / 'tests/mqtt_core/stubs', protocol, protocol / 'mico', protocol / 'MQTTPacket'],
                             (['-DMAX_MESSAGE_HANDLERS=12', '-DPH_EXPECTED_MESSAGE_HANDLERS=12'] if model != 'TC1'
                              else ['-DPH_EXPECTED_MESSAGE_HANDLERS=5']) +
                             ['-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])
    def test_a1_session_pacing(self):
        self.check_session_fixture('pacing','A1')
    def test_m1_session_pacing(self):
        self.check_session_fixture('pacing','M1')
    def test_tc1_session_pacing(self):
        self.check_session_fixture('pacing','TC1')
    def test_a1_ingress_overload(self):
        self.check_session_fixture('overload','A1')
    def test_m1_ingress_overload(self):
        self.check_session_fixture('overload','M1')
    def test_tc1_ingress_overload(self):
        self.check_session_fixture('overload','TC1')
    def test_a1_failure_evidence(self):
        self.check_session_fixture('failure_evidence','A1')
    def test_m1_failure_evidence(self):
        self.check_session_fixture('failure_evidence','M1')
    def test_tc1_failure_evidence(self):
        self.check_session_fixture('failure_evidence','TC1')
    def test_a1_session_first_cause(self):
        self.check_session_fixture('firstcause','A1')
    def test_m1_session_first_cause(self):
        self.check_session_fixture('firstcause','M1')
    def test_tc1_session_first_cause(self):
        self.check_session_fixture('firstcause','TC1')
    def check_session_fixture(self, name, model):
        with self.subTest(model=model), self.protocol(model) as protocol:
            self.compile_run([ROOT / ('tests/mqtt_core/' + name + '.c'),
                              ROOT / 'common/mqtt/core.c', ROOT / 'common/mqtt/client.c',
                              ROOT / 'common/mqtt/session.c', ROOT / 'common/mqtt/diagnostics.c', protocol / 'MQTTClient.c',
                              *sorted((protocol / 'MQTTPacket').glob('*.c'))],
                             [ROOT / 'tests/mqtt_core/stubs', protocol,
                              protocol / 'mico', protocol / 'MQTTPacket'],
                             (['-DMAX_MESSAGE_HANDLERS=12', '-DPH_EXPECTED_MESSAGE_HANDLERS=12']
                              if model != 'TC1' else ['-DPH_EXPECTED_MESSAGE_HANDLERS=5']) +
                             ['-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])
    def test_a1_ingress_self_feedback(self):
        from tests.test_http_json_budget import function
        product=(ROOT/'A1/mqtt.c').read_text()
        policy=function(product,'static bool topic_is(')+'\n'
        if 'static bool feature_ingress_topic(' in product:
            policy+=function(product,'static bool feature_ingress_topic(')+'\n'
        policy+=function(product,'static bool ingress_accept(')
        fixture=r'''
#define main existing_paho_scenarios
#include "tests/mqtt_core/paho.c"
#undef main
#define FEATURE_MQTT_A1_FILTER_MAX 4
#define FEATURE_TOPIC_MAX 512
static char base[192],feature_filters[4][512];
static unsigned births;static bool observed_retained;
static void ha_birth(MessageData*d){assert(d);births++;}
static void feature_command(MessageData*d){assert(d&&ph_mqtt_debug_core()->dispatch_depth&&!ph_mqtt_debug_core()->protocol_depth);observed_retained=d->message->retained;executions++;}
/* POLICY */
int main(void){
 existing_paho_scenarios();ph_mqtt_owner_end();
 strcpy(base,account.base);
 snprintf(feature_filters[3],512,"%s/linkage/#",base);
 assert(mqtt_link_topic(&account,"a1",account.mac,"+","response",feature_filters[0],512));
 assert(mqtt_link_topic(&account,"a1",account.mac,"+","request",feature_filters[1],512));
 assert(mqtt_aux(&account,"address","+",feature_filters[2],512));
 assert(ph_mqtt_owner_init(&account,8192,&thread,ingress_accept));init();
 for(unsigned i=0;i<4;i++)assert(!ph_mqtt_subscribe(&client,feature_filters[i],QOS1,feature_command));
 assert(!ph_mqtt_subscribe(&client,"homeassistant/status",QOS1,ha_birth));
 const char *ignored[]={"fan/state","fan/mode/state","fan/percentage/state",
  "light/state","child_lock/state","led_always_on/state","availability","average","sensor",
  "aa:bb:cc:dd:ee:ff/fan/state","aa:bb:cc:dd:ee:ff/sensor"};
 for(unsigned round=0;round<20;round++){
  for(unsigned i=0;i<sizeof(ignored)/sizeof(*ignored);i++){
   char t[512];snprintf(t,sizeof(t),"%s/linkage/%s",base,ignored[i]);incoming(t,"{}",100+i);
  }
  assert(!ph_mqtt_yield(&client,1));assert(!ph_mqtt_debug_core()->count[0]);
 }
 const char *accepted[]={"get","set","command","fan/set","fan/percentage/set","light/set",
  "child_lock/set","led_always_on/set","binding/get","binding/set","binding/state",
  "linkage/get","linkage/set","linkage/state","light/config/get","light/config/set",
  "light/config/state","schedule/get","schedule/set","schedule/state","sleep/get",
  "sleep/set","sleep/state","time/get","time/set","time/state",
  "aa:bb:cc:dd:ee:ff/command","aa:bb:cc:dd:ee:ff/light/set"};
 for(unsigned i=0;i<sizeof(accepted)/sizeof(*accepted);i++){
  char t[512];snprintf(t,sizeof(t),"%s/linkage/%s",base,accepted[i]);incoming(t,"{}",300+i);
  unsigned before=executions;assert(!ph_mqtt_yield(&client,1));
  assert(ph_mqtt_debug_core()->count[0]==1&&executions==before);
  assert(!ph_mqtt_poll(&client)&&executions==before+1);
 }
 /* RPC sample/ack envelopes and both address delivery modes remain queued.
  * Paho supplies length-delimited topic strings on this real wire path. */
 char t[512];for(unsigned i=0;i<3;i++){
  if(i<2)assert(mqtt_link_topic(&account,"a1",account.mac,"112233445566",i?"request":"response",t,sizeof(t)));
  else assert(mqtt_aux(&account,"address","112233445566",t,sizeof(t)));
  incoming(t,"{}",500+i);unsigned before=executions;
  assert(!ph_mqtt_yield(&client,1)&&!ph_mqtt_poll(&client)&&executions==before+1);
 }
 unsigned char p[2048];MQTTString retained_topic=MQTTString_initializer;retained_topic.cstring=t;
 int size=MQTTSerialize_publish(p,sizeof(p),0,1,1,600,retained_topic,(unsigned char*)"{}",2);
 assert(size>0);append(p,size);assert(!ph_mqtt_yield(&client,1)&&!ph_mqtt_poll(&client)&&observed_retained);
 incoming("homeassistant/status","online",601);assert(!ph_mqtt_yield(&client,1)&&!ph_mqtt_poll(&client)&&births==1);
 snprintf(feature_filters[3],512,"group/room/#");
 assert(!feature_ingress_topic("group/room/get",strlen("group/room/get")));
 assert(feature_ingress_topic("group/room/linkage/get",strlen("group/room/linkage/get")));
 cleanup();ph_mqtt_owner_end();assert(!native_live&&!charged_live);return 0;
}
'''.replace('/* POLICY */',policy)
        with self.protocol('A1') as protocol,tempfile.TemporaryDirectory() as d:
            source=Path(d)/'a1.c';source.write_text(fixture)
            self.compile_run([source,ROOT/'common/mqtt/core.c',ROOT/'common/mqtt/client.c',
                ROOT/'common/mqtt/session.c',protocol/'MQTTClient.c',
                *sorted((protocol/'MQTTPacket').glob('*.c'))],
                [ROOT,ROOT/'tests/mqtt_core/stubs',protocol,protocol/'mico',protocol/'MQTTPacket'],
                ['-DMAX_MESSAGE_HANDLERS=12','-DPH_EXPECTED_MESSAGE_HANDLERS=12','-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])

    def test_client_lifecycle_concurrent(self):
        with self.protocol('TC1') as protocol:
            self.compile_run([ROOT/'tests/mqtt_core/lifecycle.c',ROOT/'common/mqtt/core.c',
                ROOT/'common/mqtt/client.c',protocol/'MQTTClient.c',
                *sorted((protocol/'MQTTPacket').glob('*.c'))],
                [ROOT/'tests/mqtt_core/stubs',protocol,protocol/'mico',protocol/'MQTTPacket'], ['-pthread'])
    def test_actual_service(self):
        self.check_service_fixture()
    def test_large_service_receipt_during_dispatch(self):
        self.check_service_fixture(extra_defines=['-DLARGE_RECEIPT_FIXTURE'])
        # The invalid callback-only gate rejects this legitimate receipt growth.
        source=(ROOT/'common/mqtt/client.c').read_text()
        original='c == port.client && port.core.protocol_depth &&\n        permit(NULL, bytes)'
        blocked=source.replace(original,
            'c == port.client && port.core.protocol_depth &&\n        !port.core.dispatch_depth && permit(NULL, bytes)')
        self.assertNotEqual(source,blocked)
        with tempfile.TemporaryDirectory() as directory:
            client=Path(directory)/'client.c';client.write_text(blocked)
            with self.assertRaises(subprocess.CalledProcessError):
                self.check_service_fixture(extra_defines=['-DLARGE_RECEIPT_FIXTURE'],client_source=client)
    def test_tc1_application_conversion_sources(self):
        from tests.test_http_json_budget import function
        source=(ROOT/'TC1/mqtt_server/user_mqtt_client.c').read_text()
        body='\n'.join(function(source,n) for n in
            ('static int send_owned(', 'static int outgoing(const ph_mqtt_item *message) {'))
        fixture=r'''#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define PH_MQTT_ERROR_APP_SEND_CONVERSION 13
#define MAX_MQTT_TOPIC_SIZE 512
#define MAX_MQTT_DATA_SIZE 2048
#define QOS0 0
#define QOS1 1
#define kNoMemoryErr -4
#define kParamErr -5
#define MQTT_FAILURE -1
static struct {int mode;} account={1};static int c;
static bool synchronous_send,oom,convert=true,current=true;
static unsigned tagged,sent;static int sdk_rc;
static int ph_mqtt_app_error(unsigned stage,int rc){assert(stage==13 && rc<0);tagged++;return rc;}
static void *allocate(size_t n){return oom?NULL:malloc(n);}
#define malloc allocate
static bool account_message(const char*t,const char*b,char*x,char*y){(void)t;(void)b;strcpy(x,"topic");strcpy(y,"body");return convert;}
static int MqttMsgPublish(int*p,const char*t,int q,int r,const unsigned char*b,size_t n){(void)p;(void)t;(void)q;(void)r;(void)b;(void)n;sent++;return sdk_rc;}
typedef struct {bool retained;} ph_mqtt_item;
static const char *ph_mqtt_item_topic(const ph_mqtt_item*i){(void)i;return "homeassistant/x";}
static const char *ph_mqtt_item_payload(const ph_mqtt_item*i){(void)i;return "body";}
static bool session_current(void){return current;}
/* BODY */
int main(void){
 ph_mqtt_item i={0};oom=true;
 assert(send_owned("homeassistant/x","body",true)==-4 && !tagged && !sent);
 synchronous_send=true;
 assert(send_owned("homeassistant/x","body",true)==-4 && tagged==1 && !sent);
 oom=false;convert=false;
 assert(send_owned("homeassistant/x","body",true)==-5 && tagged==2 && !sent);
 synchronous_send=false;
 assert(send_owned("homeassistant/x","body",true)==-5 && tagged==2 && !sent);
 assert(outgoing(&i)==-5 && tagged==3 && !sent);
 convert=true;assert(outgoing(&i)==0 && tagged==3 && sent==1);
 sdk_rc=-17;assert(outgoing(&i)==-17 && tagged==4 && sent==2);
 current=false;assert(outgoing(&i)==-1 && tagged==4 && sent==2);
}
'''.replace('/* BODY */',body)
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp,'sources.c');path.write_text(fixture)
            self.compile_run([path])

    def test_a1_application_sources(self):
        from tests.test_http_json_budget import function
        source=(ROOT/'A1/mqtt.c').read_text()
        builders='\n'.join(function(source,name) for name in
            ('static void str(', 'static void topic(',
             'static __attribute__((noinline)) int filter_discovery(', 'static int discovery_build(', 'static int discovery('))
        fixture=(ROOT/'tests/mqtt_core/a1_builders.in').read_text().replace('/* BUILDERS */',builders)
        with tempfile.TemporaryDirectory() as temp:
            Path(temp,'a1_builders.inc').write_text(fixture)
            self.check_service_fixture([ROOT,Path(temp)], ['-DA1_BUILDERS_FIXTURE'])
            # Restore the pre-fix fatal allocation policy as a negative control.
            old_builders=builders.replace(
                'service_mqtt_json_error(scope, PH_MQTT_ERROR_APP_DISCOVERY_GRAPH)',
                'ph_mqtt_app_error(PH_MQTT_ERROR_APP_DISCOVERY_GRAPH,MQTT_FAILURE)')
            self.assertNotEqual(old_builders,builders)
            Path(temp,'a1_builders.inc').write_text(
                (ROOT/'tests/mqtt_core/a1_builders.in').read_text().replace('/* BUILDERS */',old_builders))
            with self.assertRaises(subprocess.CalledProcessError):
                self.check_service_fixture([ROOT,Path(temp)], ['-DA1_BUILDERS_FIXTURE'])

    def check_service_fixture(self, extra_includes=(), extra_defines=(), client_source=None):
        sdk = ROOT/'build/a1-sdk/mico-os'
        protocol=sdk/'libraries/protocols/mqtt'
        with self.protocol('A1') as protocol, tempfile.TemporaryDirectory() as directory:
            json=copy_patched_json(sdk/'libraries/utilities/json_c',Path(directory)/'sdk')
            for p in json.glob('*.c'): p.write_text(p.read_text().replace('%ld','%d'))
            bear=ROOT/'build/ota-tls/bearssl-0.6'
            self.compile_run([ROOT/'tests/mqtt_core/service.c',ROOT/'common/mqtt/core.c',
                client_source or ROOT/'common/mqtt/client.c',ROOT/'common/mqtt/session.c',ROOT/'common/mqtt/service.c',
                ROOT/'common/json_owned.c', protocol/'MQTTClient.c',
                *sorted((protocol/'MQTTPacket').glob('*.c')), *json.glob('*.c'),
                *[bear/'src'/p for p in ('hash/sha2small.c','codec/dec32be.c','codec/enc32be.c','codec/enc64be.c')]],
                [ROOT/'tests/mqtt_core/stubs',ROOT/'tests/stubs',protocol,protocol/'mico',protocol/'MQTTPacket',json,bear/'inc',bear/'src',*extra_includes],
                [*extra_defines,'-include','stdint.h','-DMOC=1','-DMAX_MESSAGE_HANDLERS=12','-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])
    def test_a1_protocol(self): self.check_protocol('A1')
    def test_m1_protocol(self): self.check_protocol('M1')
    def test_tc1_protocol(self): self.check_protocol('TC1')

    def test_a1_product_publish_quota_diagnostics(self): self.check_product_publish_quota_diagnostics('A1')
    def test_m1_product_publish_quota_diagnostics(self): self.check_product_publish_quota_diagnostics('M1')
    def check_product_publish_quota_diagnostics(self, model):
        """Real adapter functions, shared turn budget and pinned Paho; no sockets."""
        from tests.test_http_json_budget import function
        fixture = r'''
#define main existing_paho_scenarios
#define publish fixture_paho_publish
#include "tests/mqtt_core/paho.c"
#undef publish
#undef main
static struct {
 unsigned mqtt_errors;
 char mqtt_last_error[96];
 bool mqtt_connected;
} PRODUCT_state;
static mico_mutex_t PRODUCT_lock;
/* PRODUCTION */
int main(void) {
 assert(mqtt_account_init(&account,1,"MODEL","test","aabbccddeeff"));
 assert(ph_mqtt_owner_init(&account,8192,&thread,accept));
 init();
 PRODUCT_state.mqtt_connected=true;
 /* Code 2 remains an error for operations such as a refused CONNECT. */
 mqtt_error("connect",2);
 assert(PRODUCT_state.mqtt_errors==1);
 assert(!strcmp(PRODUCT_state.mqtt_last_error,"connect: 2"));
 ph_mqtt_diagnostics before,after;
 ph_mqtt_snapshot(&before);
 unsigned sent=publications;
 ph_mqtt_turn_begin(4);
 for(unsigned i=0;i<4;i++)assert(publish("tx/state","state",true)==MQTT_SUCCESS);
 assert(publish("tx/state","deferred",true)==PH_MQTT_MORE);
 ph_mqtt_turn_end();
 ph_mqtt_snapshot(&after);
 assert(publications==sent+4 && after.errors==before.errors);
 assert(PRODUCT_state.mqtt_errors==1 && PRODUCT_state.mqtt_connected);
 assert(!strcmp(PRODUCT_state.mqtt_last_error,"connect: 2"));
 ph_mqtt_turn_begin(4);
 assert(publish("tx/state","next turn",true)==MQTT_SUCCESS);
 ph_mqtt_turn_end();
 assert(publications==sent+5 && PRODUCT_state.mqtt_errors==1);
 assert(publish("tx/state",NULL,true)==MQTT_FAILURE);
 assert(PRODUCT_state.mqtt_errors==2);
 assert(!strcmp(PRODUCT_state.mqtt_last_error,"serialize: -1"));
 size_t saved_capacity=heap_capacity;
 heap_capacity=charged_live+16384u+32u;
 assert(publish("tx/state","no headroom",true)==PH_MQTT_MORE);
 assert(PRODUCT_state.mqtt_errors==2);
 now_ms+=30000;
 assert(publish("tx/state","no headroom",true)==MQTT_FAILURE);
 assert(PRODUCT_state.mqtt_errors==3);
 assert(strstr(PRODUCT_state.mqtt_last_error,": -1"));
 heap_capacity=saved_capacity;
 cleanup();init();
 fail_send=true;
 assert(publish("tx/state","failed",true)==MQTT_SOCKET_ERR);
 fail_send=false;
 assert(PRODUCT_state.mqtt_errors==4);
 assert(strstr(PRODUCT_state.mqtt_last_error,": -3"));
 cleanup();
 init();
 assert(publish("tx/state","recovered",true)==MQTT_SUCCESS);
 assert(PRODUCT_state.mqtt_errors==4);
 cleanup();ph_mqtt_owner_end();
 assert(!native_live && !charged_live);
 return 0;
}
'''

        with self.subTest(model=model), self.protocol(model) as protocol, tempfile.TemporaryDirectory() as d:
            product = (ROOT/model/'mqtt.c').read_text()
            functions = (function(product, 'static void mqtt_error(') + '\n' +
                         function(product, 'static int publish('))
            source = Path(d)/'diagnostics.c'
            source.write_text(fixture.replace('/* PRODUCTION */', functions)
                              .replace('PRODUCT', model.lower()).replace('MODEL', model.lower()))
            self.compile_run([source, ROOT/'common/mqtt/core.c', ROOT/'common/mqtt/client.c',
                              ROOT/'common/mqtt/session.c', protocol/'MQTTClient.c',
                              *sorted((protocol/'MQTTPacket').glob('*.c'))],
                             [ROOT, ROOT/'tests/mqtt_core/stubs', protocol, protocol/'mico',
                              protocol/'MQTTPacket'],
                             ['-DMAX_MESSAGE_HANDLERS=12', '-DPH_EXPECTED_MESSAGE_HANDLERS=12',
                              '-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])
if __name__ == '__main__': unittest.main()
