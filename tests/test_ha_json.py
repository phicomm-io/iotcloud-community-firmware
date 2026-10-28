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
        code = code[code.index('enum { MQTT_BRIGHTNESS_INDEX'):code.index('static bool discovery_deferred;')]
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
