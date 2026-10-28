"""HA-only bounded writer and real M1 discovery output coverage."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HaJson(unittest.TestCase):
    def run_c(self, source):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'test.c'
            exe = Path(d) / 'test'
            src.write_text(source)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                            '-I', str(ROOT), str(src), '-o', str(exe)], check=True)
            return subprocess.check_output([str(exe)], text=True)

    def test_bounds_escaping_and_integer_limits(self):
        self.run_c(r'''
#include "common/ha_json.h"
#include <assert.h>
static void emit(ha_json *w){
 ha_json_begin(w,0,false);ha_json_string(w,"x","\"\\\n\001中文");
 ha_json_int(w,"n",INT64_MIN);ha_json_begin(w,"a",true);
 ha_json_bool(w,0,true);ha_json_end(w);ha_json_end(w);
}
int main(void){
 char bytes[256];ha_json w;ha_json_init(&w,bytes,sizeof(bytes));emit(&w);
 assert(ha_json_done(&w));size_t length=w.length;
 assert(!strcmp(bytes,"{\"x\":\"\\\"\\\\\\u000a\\u0001中文\",\"n\":-9223372036854775808,\"a\":[true]}"));
 for(size_t n=0;n<=length;n++){ha_json_init(&w,bytes,n);emit(&w);assert(!ha_json_done(&w));}
 ha_json_init(&w,bytes,length+1);emit(&w);assert(ha_json_done(&w));
 ha_json_init(&w,bytes,sizeof(bytes));for(unsigned i=0;i<13;i++)ha_json_begin(&w,0,true);assert(!w.valid);
 ha_json_init(&w,bytes,sizeof(bytes));ha_json_begin(&w,0,false);ha_json_string(&w,0,"invalid");assert(!w.valid);
 return 0;
}''')

    def test_m1_discovery_payloads(self):
        code = (ROOT / 'M1/mqtt.c').read_text()
        code = code[code.index('enum { MQTT_BRIGHTNESS_INDEX'):code.index('static int ha_particles(')]
        prelude = r'''
#include "common/ha_json.h"
#include <stdio.h>
#include <stdlib.h>
#define MQTT_TOPIC_MAX 256
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 1
#define M1_BAROMETER_STALE_MS 20000
#define M1_VERSION "rc4"
typedef struct {char name[64];} m1_config_t;
static int m1_lock,account;
static struct {struct {bool received;} scd40,barometer;bool mcu_sensor_details;} m1_state={ {true},{true},true};
static const char *base="device",*availability_topic="device/availability",*command_topic="device/ha/command";
static void mico_rtos_lock_mutex(int *p){(void)p;}
static void mico_rtos_unlock_mutex(int *p){(void)p;}
static int device_language_get(void){return 1;}
static const char *ph_i18n(int lang,const char *zh,const char *en){return lang?en:zh;}
static void mqtt_leaf(int *a,const char *leaf,char *out,size_t n){(void)a;snprintf(out,n,"device/%s",leaf);}
static void mqtt_identity(int *a,const char *leaf,char *out,size_t n){(void)a;snprintf(out,n,"uid_%s",leaf);}
static void mqtt_ha(int *a,const char *component,const char *key,char *out,size_t n){(void)a;snprintf(out,n,"ha/%s/%s",component,key);}
static int publish(const char *topic,const char *body,bool retain){(void)retain;printf("%s\t%s\n",topic,body);return 0;}
'''
        output = self.run_c(prelude + code + r'''
int main(void){m1_config_t c={"Name\"中文"};for(unsigned i=0;i<MQTT_DISCOVERY_COUNT;i++){
char bytes[3072];ha_json w;ha_json_init(&w,bytes,sizeof(bytes));if(discovery_build(&c,i,&w))return 1;}
return 0;}
''')
        lines = output.splitlines()
        self.assertEqual(len(lines), 15)
        for line in lines:
            topic, raw = line.split('\t')
            data = json.loads(raw)
            key = topic.split('/')[-1]
            self.assertEqual(data['unique_id'], 'uid_' + key)
            self.assertEqual(data['device']['name'], 'Name"中文')
            self.assertEqual(data['device']['identifiers'], ['uid_'])
            self.assertFalse(data['retain'])
            if key == 'brightness':
                self.assertEqual(data['command_topic'], 'device/ha/command')
                self.assertEqual(data['max'], 100)
            else:
                self.assertEqual(len(data['availability']), 2)
                self.assertEqual(data['availability_mode'], 'all')

    def test_a1_discovery_matrix(self):
        source = (ROOT / 'A1/mqtt.c').read_text()
        code = source[source.index('static void topic(ha_json'):source.index('static void primary_snapshot(')]
        code += source[source.index('static int discovery_build(const a1_config_t *c'):source.index('static int ha_state(')]
        prelude = r'''
#include "common/ha_json.h"
#include <stdio.h>
#include <stdlib.h>
#define MQTT_TOPIC_MAX 256
#define FEATURE_TOPIC_MAX 256
#define MQTT_SUCCESS 0
#define MQTT_FAILURE -1
#define PH_MQTT_MORE 1
#define PH_MQTT_ERROR_APP_DISCOVERY_IDENTITY 1
#define A1_VERSION "rc4"
#define A1_FILTER_MAX_HOURS 20000
#define A1_FRESH_AIR
typedef struct {char name[64];} a1_config_t;
static int account;
static bool bound;
static const char *base="device",*availability_topic="device/availability";
static bool feature_bound(void){return bound;}
static bool feature_ha_device(char *id,size_t n,char *name,size_t m,bool *grouped){snprintf(id,n,"parent");snprintf(name,m,"Parent");*grouped=true;return true;}
static int ph_mqtt_app_error(int a,int b){(void)a;return b;}
static int device_language_get(void){return 1;}
static const char *ph_i18n(int lang,const char *zh,const char *en){return lang?en:zh;}
static void mqtt_identity(int *a,const char *leaf,char *out,size_t n){(void)a;snprintf(out,n,"uid_%s",leaf);}
static void mqtt_ha(int *a,const char *component,const char *key,char *out,size_t n){(void)a;snprintf(out,n,"ha/%s/%s",component,key);}
static int publish(const char *topic,const char *body,bool retain){(void)retain;printf("%s\t%s\n",topic,body);return 0;}
'''
        output = self.run_c(prelude + code + r'''
int main(void){a1_config_t c={"Name\"中文"};for(unsigned mode=0;mode<2;mode++){bound=mode;
for(unsigned i=0;i<12;i++){char bytes[3072];ha_json w;ha_json_init(&w,bytes,sizeof(bytes));if(discovery_build(&c,i,&w))return 1;}}
return 0;}
''')
        rows = output.splitlines()
        self.assertEqual(len(rows), 24)
        for i, line in enumerate(rows):
            topic, raw = line.split('\t')
            key = topic.split('/')[-1]
            if i >= 12 and i % 12 < 4:
                self.assertEqual(raw, '')
                continue
            data = json.loads(raw)
            self.assertEqual(data['unique_id'], 'uid_' + key)
            self.assertIn(data['device']['identifiers'], [['parent'], ['uid_']])
            self.assertLess(len(raw.encode()), 3072)
            if key == 'fan_mode':
                self.assertEqual(data['options'], ['Automatic', 'Manual'])
            if key == 'fan':
                self.assertEqual(data['preset_modes'], ['auto', 'manual'])

    def test_ha_command_restricted_parser(self):
        self.run_c(r''' 
#include <assert.h>
#include "common/ha_command.h"
#include "common/typed_wire.c"
#include "common/wire_buffer.c"
static unsigned calls;
ph_buf *service_command_mp(const void *p,size_t n){ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,p,n,&v));calls++;return NULL;}
int main(void){
 unsigned char bytes[1536];
 const char *valid[]={"{\"action\":\"light.set\",\"params\":{\"color\":{\"r\":255},\"enabled\":true}}","{\"action\":\"fan.set\",\"params\":{\"on\":false,\"speed\":100}}","{\"action\":\"x\",\"params\":{\"text\":\"\\uD83D\\uDE00\"}}",NULL};
 for(unsigned i=0;valid[i];i++){size_t len=strlen(valid[i]);assert(ha_command_pack(valid[i],len,bytes,sizeof(bytes)));for(size_t n=0;n<len;n++)assert(!ha_command_pack(valid[i],n,bytes,sizeof(bytes)));ha_command_input(valid[i],len);}
 assert(calls==3);
 const char *bad[]={"{\"a\":1,\"a\":2}","{\"x\":01}","{\"x\":1.0}","{\"x\":null}","{\"x\":[]}","{\"x\":9007199254740992}","{\"x\":\"\\u0000\"}","{\"x\":\"\\uD800\"}","{} trailing",NULL};
 for(unsigned i=0;bad[i];i++)assert(!ha_command_pack(bad[i],strlen(bad[i]),bytes,sizeof(bytes)));
 return 0;
}
''')

    def test_tc1_discovery_matrix(self):
        output = self.run_c(r'''
#include "common/ha_json.h"
#include "common/mqtt_account.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#define MAX_MQTT_DATA_SIZE 2048
#define PH_MQTT_MORE 1
#define VERSION "rc4"
static mqtt_account_t account;
static bool synchronous_send;
static int synchronous_error;
static struct {struct{unsigned language;}bemfa;char socket_names[6][64];} config;
static __typeof__(config) *user_config=&config;
static struct {struct{char name[64];}micoSystemConfig;} system_data;
static __typeof__(system_data) *sys_config=&system_data;
static const char *ph_i18n(unsigned l,const char *zh,const char *en){return l?en:zh;}
static int UserMqttSendTopic(char *topic,char *body,char retained){assert(retained);printf("%s\t%s\n",topic,body);return 0;}
#include "TC1/mqtt_server/ha_discovery.inc"
int main(void){
 assert(mqtt_account_init(&account,1,"tc1","user","aabbccddeeff"));
 strcpy(sys_config->micoSystemConfig.name,"Device\"中文");config.bemfa.language=1;
 for(unsigned i=0;i<17;i++)tc1_ha_discovery(i);
 return 0;
}
''')
        rows = output.splitlines()
        self.assertEqual(len(rows), 17)
        ids = set()
        for row in rows:
            topic, raw = row.split('\t')
            data = json.loads(raw)
            self.assertEqual(data['device']['name'], 'Device"中文')
            self.assertNotIn(data['uniq_id'], ids)
            ids.add(data['uniq_id'])
            if 'cmd_t' in data:
                self.assertTrue(data['cmd_t'].endswith('/ha/command'))
            else:
                self.assertTrue(data['state_topic'].endswith('/ha/sensor'))
