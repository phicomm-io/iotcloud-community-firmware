"""Execute actual owner-local producer bodies against the native MP decoder."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class SensorProducers(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as td:
            src=Path(td)/'test.c'; exe=Path(td)/'test';src.write_text(code)
            subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                '-fsanitize=address,undefined','-I',str(ROOT),str(src),str(ROOT/'common/typed_wire.c'),
                str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_fan_and_power_metadata_retry_then_state(self):
        for model,path,prefix,end,call in [('a1','A1/mqtt.c','fan','static int discovery_build','fan_sensor_cycle(true,80,&handled)'),
            ('tc1','TC1/mqtt_server/user_mqtt_client.c','power','static bool power_requested','power_sensor_cycle(&handled)')]:
            with self.subTest(model=model):
                text=(ROOT/path).read_text()
                start=text.index('static uint32_t '+prefix+'_sensor_boot')
                body=text[start:text.index(end,start)]
                if model=='a1':
                    # HAL readiness is independently tested; no mutex emulation needed here.
                    a=body.index('static bool fan_sensor_ready');b=body.index('static int fan_sensor_cycle',a)
                    body=body[:a]+'static bool fan_sensor_ready(void){return false;}\n'+body[b:]
                self.compile_run(r'''
#include "common/sensor_mp.h"
#include "common/mqtt_account.h"
#include <assert.h>
#include <string.h>
#define MQTT_SUCCESS 0
#define kNoErr 0
static mqtt_account_t account;
static uint32_t now=1;
static int calls,fail;
static bool last_retained;
static char last_topic[256];
static unsigned char last_body[2048];static size_t last_length;
static uint32_t mico_rtos_get_time(void){return now;}
static int MicoRandomNumberRead(uint8_t *out,size_t n){assert(n==4);uint32_t value=23;memcpy(out,&value,4);return 0;}
typedef struct{float watts;bool valid;uint32_t age_ms;} tc1_power_sample_t;
static void __attribute__((unused)) PowerSampleRead(tc1_power_sample_t *s){s->watts=51.25f;s->valid=true;s->age_ms=20000;}
static int send_bytes(const char *topic,const void *bytes,size_t n,bool retained){
 assert(n<sizeof(last_body));strcpy(last_topic,topic);memcpy(last_body,bytes,n);last_length=n;last_retained=retained;calls++;return fail?-1:0;
}
#define publish_bytes send_bytes
#define service_sender send_bytes
''' + body + f'''
int main(void){{
 assert(mqtt_account_init(&account,1,"{model}","user","aabbccddeeff"));
 bool handled;fail=1;assert({call}==-1);assert(handled&&calls==1&&last_retained);
 {prefix}_sensor_turn=false;now+=1000;fail=0;assert({call}==0);assert(calls==2&&last_retained);
 ph_value root,field,value;assert(phv_parse(PH_WIRE_MSGPACK,last_body,last_length,&root));
 assert(phv_get(&root,"fields",&field)&&field.kind==PHV_MAP);assert(strstr(last_topic,"/metadata"));
 {prefix}_sensor_turn=false;assert({call}==0);assert(calls==3&&!last_retained);assert(strstr(last_topic,"/state"));
 assert(phv_parse(PH_WIRE_MSGPACK,last_body,last_length,&root));assert(phv_get(&root,"sensors",&field));
 assert(phv_get(&field,"{'power' if model=='tc1' else 'fan_speed_percent'}",&value));
 field=value;assert(phv_get(&field,"value",&value)&&value.kind==PHV_NULL);
 int64_t seq;assert(phv_get(&root,"sequence",&value)&&phv_int(&value,&seq)&&seq==1);
 {prefix}_sensor_turn=false;now+=100;assert({call}==0);assert(calls==3);
 return 0;
}}
''')

    def test_m1_optional_hardware_revision_and_session_metadata(self):
        text=(ROOT/'M1/mqtt.c').read_text()
        body=text[text.index('static int publish_typed_sensors'):text.index('\n#include "../common/mqtt/service.h"',text.index('static int publish_typed_sensors'))]
        self.compile_run(r'''
#include "common/sensor_mp.h"
#include "common/mqtt_account.h"
#include <assert.h>
#include <string.h>
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 1
#define kNoErr 0
static mqtt_account_t account;
static uint32_t sensor_boot,sensor_sequence,sensor_at,sensor_revision=1;
static bool sensor_started,sensor_metadata;static unsigned sensor_mask;
static unsigned calls;static bool last_retained;static unsigned char bytes[2048];static size_t length;
typedef struct{bool received;uint32_t updated_ms;int temperature_deci_c,humidity_deci_pct,hcho_ug_m3,pm25_ug_m3;}m1_sample_t;
typedef struct{bool received,ready;uint32_t updated_ms;int pressure_centi_pa;}pressure_t;
typedef struct{m1_sample_t sample;pressure_t barometer;struct{bool ready,received;uint32_t updated_ms;int co2_ppm;}scd40;}mqtt_state_t;
static bool m1_sample_fresh(const m1_sample_t *s,uint32_t now){return s->received&&now-s->updated_ms<20000;}
static unsigned m1_sample_fields(const m1_sample_t *s){(void)s;return 15;}
static bool m1_barometer_fresh(const pressure_t *s,uint32_t now){return s->received&&s->ready&&now-s->updated_ms<20000;}
static int MicoRandomNumberRead(uint8_t *out,size_t n){assert(n==4);uint32_t v=23;memcpy(out,&v,4);return 0;}
static int publish_bytes(const char *topic,const void *data,size_t n,bool retained){
 assert(strstr(topic,retained?"/metadata":"/state"));assert(n<sizeof(bytes));memcpy(bytes,data,n);length=n;last_retained=retained;calls++;return 0;}
static ph_sensor_view view;
static void check(unsigned fields,uint32_t revision,bool metadata){ph_value root;assert(phv_parse(PH_WIRE_MSGPACK,bytes,length,&root));assert(ph_sensor_view_parse(&root,account.mac,metadata,&view));assert(view.fields.count==fields&&view.descriptor_revision==revision);}
''' + body + r'''
int main(void){
 assert(mqtt_account_init(&account,1,"m1","user","aabbccddeeff"));
 mqtt_state_t s={0};s.sample.received=true;s.sample.temperature_deci_c=-53;s.sample.updated_ms=100;
 assert(!publish_typed_sensors(&s,100));assert(calls==1&&last_retained);check(4,1,true);
 assert(ph_sensor_descriptor_matches(&view,"temperature","°C"));
 assert(!publish_typed_sensors(&s,101));assert(calls==2&&!last_retained);check(4,1,false);
 float number;uint32_t age;assert(ph_sensor_reading(&view,"temperature",20000,&number,&age)&&number<-5.29f&&number>-5.31f);
 s.scd40.ready=true;s.scd40.received=true;s.scd40.updated_ms=5101;s.scd40.co2_ppm=700;
 assert(!publish_typed_sensors(&s,5101));assert(calls==3&&last_retained);check(5,2,true);
 assert(ph_sensor_descriptor_matches(&view,"co2","ppm"));
 assert(!publish_typed_sensors(&s,5102));check(5,2,false);
 s.scd40.ready=false;s.scd40.received=false;s.barometer.ready=true;s.barometer.received=true;
 assert(!publish_typed_sensors(&s,10102));check(5,3,true);assert(ph_sensor_descriptor_matches(&view,"pressure","Pa"));
 /* A reconnect resets metadata pending without resetting process boot/revision. */
 sensor_metadata=false;sensor_started=false;assert(!publish_typed_sensors(&s,10103));check(5,3,true);
 return 0;
}
''')
