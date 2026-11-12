"""Bemfa native MP configuration, POD control and canonical state reports."""
from pathlib import Path
import tempfile
from tests import test_shared_mqtt as shared
ROOT=shared.ROOT
from tests.test_status_views import function
class BemfaConfig(__import__('unittest').TestCase):
    def test_canonical_reports(self):
        source=(ROOT/'common/mqtt/bemfa.c').read_text()
        config=(ROOT/'common/mqtt/bemfa_config.c').read_text()
        body=function(source,'static int bemfa_report_one(')+'\n'+ '\n'.join(function(config,n) for n in ('unsigned bemfa_gear_speed(', 'unsigned bemfa_fan_gear('))
        fixture=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bemfa.h"
#include "common/device_status.h"
#define MQTT_SUCCESS 0
#define PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED -2
#define QOS0 0
typedef struct {unsigned qos;void *payload;size_t payloadlen;} MQTTMessage;
static struct {bemfa_config_t config;char reported[6][16];unsigned cursor,report_at;int client;} state,*worker=&state;
static unsigned report_dirty;
static device_status_t status;
static unsigned calls;static char sent[16],topic[80];
unsigned mico_rtos_get_time(void){return 100;}
static bool bemfa_capture(device_status_t *s){*s=status;return true;}
static int bm_publish(int *c,const char *t,MQTTMessage *m){assert(c==&worker->client&&m->qos==QOS0);calls++;strcpy(topic,t);memcpy(sent,m->payload,m->payloadlen);sent[m->payloadlen]=0;return 0;}
PRODUCTION
static void report(unsigned i,const char *expected){worker->cursor=i;worker->report_at=0;worker->reported[i][0]=0;assert(!bemfa_report_one());assert(!strcmp(sent,expected));char t[80];sprintf(t,"%s/up",worker->config.topics[i]);assert(!strcmp(topic,t));}
int main(void){for(unsigned i=0;i<CHANNELS;i++)sprintf(worker->config.topics[i],"topic%u",i);
#ifdef BEMFA_TC1
 for(unsigned i=0;i<6;i++){status.control.tc1.states[i]=1;report(i,"on");status.control.tc1.states[i]=0;report(i,"off");}
 status.control.tc1.states[0]=-1;unsigned before=calls;worker->cursor=0;worker->report_at=0;assert(!bemfa_report_one()&&calls==before);
#else
 status.control.a1.effective_on=true;status.control.a1.effective_speed=50;report(0,"on#2");
 status.control.a1.effective_speed=0;report(0,"off");
 status.control.a1.led_visible=false;status.control.a1.brightness=255;report(1,"off");
 status.control.a1.led_visible=true;status.control.a1.brightness=1;report(1,"on#1");
 status.control.a1.brightness=128;report(1,"on#50");status.control.a1.brightness=255;report(1,"on#100");
 unsigned before=calls;worker->cursor=1;worker->report_at=0;assert(!bemfa_report_one()&&calls==before);
#endif
 puts("Bemfa canonical actual state reports and /up route PASS");}
'''.replace('PRODUCTION',body)
        with tempfile.TemporaryDirectory() as directory:
            probe=Path(directory)/'report.c';probe.write_text(fixture)
            for flags in (['-DCHANNELS=2'],['-DBEMFA_TC1=1','-DCHANNELS=6']):
                shared.SharedMQTT().compile_run([probe],[ROOT,ROOT/'tests/stubs'],flags)

    def test_product_command_mapping(self):
        from tests.test_bemfa_mp import BemfaMp
        BemfaMp().test_deferred_owned_pod_dispatch()

    def test_tc1_append_only_migration(self):
        source=(ROOT/'TC1/main.c').read_text()
        body=function((ROOT/'common/local_auth.c').read_text(),'void ph_auth_store_empty(')+'\n'+'\n'.join(function(source,n) for n in ('uint32_t app_config_legacy_size(', 'uint32_t app_config_legacy_next_size(', 'bool app_config_migrate_legacy('))
        with tempfile.TemporaryDirectory() as directory:
            temporary=Path(directory)
            (temporary/'TC1').mkdir()
            # Preserve the actual public storage declarations; generated build
            # version is irrelevant to the persistence ABI under test.
            header=(ROOT/'TC1/main.h').read_text().replace('#include "../common/generated/build_version.h"','#define FIRMWARE_VERSION "host"')
            (temporary/'TC1/main.h').write_text(header)
            probe=temporary/'migration.c'
            probe.write_text('#include "mico.h"\n#include "mico_system.h"\n#include "TC1/main.h"\n#include "TC1/config_legacy.h"\n'+body+r'''
int main(void){user_config_t c;memset(&c,0xa5,sizeof(c));c.version=USER_CONFIG_VERSION;
 unsigned size=offsetof(user_config_t,bemfa);unsigned char before[sizeof(c)];memcpy(before,&c,sizeof(c));
 assert(app_config_legacy_size()==sizeof(user_config_v12_t));assert(app_config_legacy_next_size(sizeof(user_config_v12_t))==size);assert(app_config_migrate_legacy(&c,size));
 assert(!memcmp(before,&c,size));bemfa_config_t empty={0};assert(!memcmp(&c.bemfa,&empty,sizeof(empty)));
 assert(!app_config_migrate_legacy(&c,sizeof(c)));assert(app_config_legacy_next_size(size)==sizeof(user_config_v11_t));
 assert(app_config_legacy_next_size(sizeof(user_config_v11_t))==sizeof(user_config_v10_t));
 puts("TC1 old v12 extension migration preserves the entire original prefix PASS");}
''')
            shared.SharedMQTT().compile_run([probe],[temporary,ROOT,ROOT/'TC1',ROOT/'tests/host',ROOT/'tests/stubs'])
    def test_protocol_and_config(self):
        import ast,json,re
        def pack(v):
            if isinstance(v,bool):return bytes([0xc3 if v else 0xc2])
            if isinstance(v,int):return bytes([v])
            if isinstance(v,str):
                b=v.encode();return (bytes([0xa0+len(b)]) if len(b)<32 else bytes([0xd9,len(b)]))+b
            if isinstance(v,list):return bytes([0x90+len(v)])+b''.join(map(pack,v))
            return bytes([0x80+len(v)])+b''.join(pack(k)+pack(x) for k,x in v.items())
        source=(ROOT/'tests/mqtt_core/bemfa_config.c').read_text()
        source=source.replace('#include "json.h"','#include "common/typed_wire.h"')
        source=source.replace('assert(!bemfa_config_valid(&c,true));','assert(bemfa_config_valid(&c,true));') # rejected native updates preserve the old valid config
        a=source.index('int json_heap_permit');b=source.index('int main(void)',a)
        source=source[:a]+r'''
static void values(bemfa_config_t *c,const void *bytes,size_t n,bool valid){
 ph_value v;bemfa_config_t before=*c;assert(phv_parse(1,bytes,n,&v));assert(bemfa_config_values_mp(c,&v,true)==valid);if(!valid)assert(!memcmp(c,&before,sizeof(before)));
}
'''+source[b:]
        def convert(match):
            binary=pack(json.loads(ast.literal_eval(match[1])))
            literal='"'+''.join('\\x%02x'%b for b in binary)+'"'
            return 'values(&c,'+literal+','+str(len(binary))+','+match[2]+');'
        source=re.sub(r'values\(&c,("(?:\\.|[^"\\])*"),(true|false)\);',convert,source)
        with tempfile.TemporaryDirectory() as directory:
            probe=Path(directory)/'config.c';probe.write_text(source)
            shared.SharedMQTT().compile_run([probe,ROOT/'common/mqtt/bemfa_config.c',ROOT/'common/typed_wire.c',ROOT/'common/wire_buffer.c'],[ROOT,ROOT/'tests/stubs'])
