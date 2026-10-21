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
                'M1/mqtt-packet-deadline.patch', 'common/mqtt-response-deadline.patch')])
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
        sdk = ROOT/'build/a1-sdk/mico-os'
        protocol=sdk/'libraries/protocols/mqtt'
        with self.protocol('A1') as protocol, tempfile.TemporaryDirectory() as directory:
            json=copy_patched_json(sdk/'libraries/utilities/json_c',Path(directory)/'sdk')
            for p in json.glob('*.c'): p.write_text(p.read_text().replace('%ld','%d'))
            bear=ROOT/'build/ota-tls/bearssl-0.6'
            self.compile_run([ROOT/'tests/mqtt_core/service.c',ROOT/'common/mqtt/core.c',
                ROOT/'common/mqtt/client.c',ROOT/'common/mqtt/session.c',ROOT/'common/mqtt/service.c',
                ROOT/'common/json_owned.c', protocol/'MQTTClient.c',
                *sorted((protocol/'MQTTPacket').glob('*.c')), *json.glob('*.c'),
                *[bear/'src'/p for p in ('hash/sha2small.c','codec/dec32be.c','codec/enc32be.c','codec/enc64be.c')]],
                [ROOT/'tests/mqtt_core/stubs',ROOT/'tests/stubs',protocol,protocol/'mico',protocol/'MQTTPacket',json,bear/'inc',bear/'src'],
                ['-include','stdint.h','-DMOC=1','-DMAX_MESSAGE_HANDLERS=12','-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=strdup'])
    def test_a1_protocol(self): self.check_protocol('A1')
    def test_m1_protocol(self): self.check_protocol('M1')
    def test_tc1_protocol(self): self.check_protocol('TC1')
if __name__ == '__main__': unittest.main()
