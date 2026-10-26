"""Execute the bounded production encoder and M1 owner publisher, with sanitizers."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NumericFeed(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='numeric-feed-')
        cls.addClassCleanup(cls.tmp.cleanup)
        build = Path(cls.tmp.name)
        source = (ROOT / 'M1/mqtt.c').read_text()
        # Copy complete production definitions, not a reimplementation of policy.
        state = source[source.index('typedef struct {'):source.index('static void mqtt_state_get(')]
        producer = source[source.index('static int publish_typed_sensors('):source.index('\n#include "../common/mqtt/service.h"', source.index('static int publish_typed_sensors('))]
        code = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include "common/numeric_feed.h"
#include "M1/protocol.h"
#include "M1/barometer.h"
#define MQTT_TOPIC_MAX 192
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 17
#define kNoErr 0
int json_number_format_float32(char *s,unsigned n,float v){return snprintf(s,n,"%.9g",v);}
static struct {char mac[13];} account={"aabbccddeeff"};
static uint32_t sensor_boot,sensor_sequence,sensor_at;
static bool sensor_started;
static int random_calls,random_fail,published,publish_result;
static int MicoRandomNumberRead(uint8_t *p,size_t n){
    assert(n==4);++random_calls;if(random_fail)return -1;
    uint32_t x=UINT32_MAX;memcpy(p,&x,4);return 0;
}
static bool mqtt_format(char *s,size_t n,const char *fmt,...){
    va_list ap;va_start(ap,fmt);int r=vsnprintf(s,n,fmt,ap);va_end(ap);
    return r>=0 && (size_t)r<n;
}
static bool mqtt_aux(const void *a,const char *kind,const char *id,char *s,size_t n){
    (void)a;return mqtt_format(s,n,"phicomm/v2/owner/%s/%s",kind,id);
}
static int publish(const char *topic,const char *payload,bool retained){
    assert(!retained);assert(!strcmp(topic,"phicomm/v2/owner/sensor/aabbccddeeff/state"));
    ++published;puts(payload);return publish_result;
}
bool m1_sample_fresh(const m1_sample_t *s,uint32_t now){return s->received&&(uint32_t)(now-s->updated_ms)<M1_STALE_MS;}
''' + state + producer + r'''
int main(int argc,char **argv){
    assert(argc==2);mqtt_state_t s={0};
    s.sample=(m1_sample_t){.received=true,.updated_ms=1000,.temperature_deci_c=-35,
        .humidity_deci_pct=523,.hcho_ug_m3=127,.pm25_ug_m3=28,.climate_status=1,.pm25_status=1};
    s.scd40.ready=s.scd40.received=true;s.scd40.co2_ppm=750;s.scd40.updated_ms=1000;
    s.barometer=(barometer_state_t){.ready=true,.received=true,.pressure_centi_pa=10001234,.updated_ms=1000};
    if(!strcmp(argv[1],"fresh")){
        assert(!publish_typed_sensors(&s,1000));assert(published==1);
        assert(!publish_typed_sensors(&s,1001));assert(published==1);
        s.sample.climate_status=0;s.sample.pm25_status=0;
        assert(!publish_typed_sensors(&s,6000));assert(published==2);
        assert(!publish_typed_sensors(&s,26000));assert(published==3);
    }else if(!strcmp(argv[1],"missing")){
        memset(&s,0,sizeof(s));assert(!publish_typed_sensors(&s,0));
        assert(!publish_typed_sensors(&s,1));assert(published==1);
    }else if(!strcmp(argv[1],"random")){
        random_fail=1;publish_typed_sensors(&s,1000);publish_typed_sensors(&s,1001);
        assert(random_calls==1 && !published);random_fail=0;
        publish_typed_sensors(&s,6000);assert(random_calls==2 && published==1);
    }else if(!strcmp(argv[1],"wrap")){
        sensor_sequence=UINT32_MAX-1;publish_typed_sensors(&s,UINT32_MAX-100);
        publish_typed_sensors(&s,4899);assert(published==2 && sensor_sequence==1);
    }else if(!strcmp(argv[1],"publish_error")){
        publish_result=-7;assert(publish_typed_sensors(&s,1000)==-7);
        assert(sensor_sequence==1 && published==1);
        assert(!publish_typed_sensors(&s,1001) && published==1);
        publish_result=PH_MQTT_MORE;assert(!publish_typed_sensors(&s,6000));
        assert(sensor_sequence==2 && published==2);
        assert(!publish_typed_sensors(&s,6001) && published==2);
        publish_result=0;assert(!publish_typed_sensors(&s,11000));
        assert(sensor_sequence==3 && published==3);
    }else if(!strcmp(argv[1],"encode")){
        numeric_feed_field_t fields[]={
            {"custom_value","Quoted \"name\" \\ 温度","custom",1.25f,17,true},
            {"not_available","Unavailable","",NAN,0,true},
            {"disabled","Disabled","W",123,9,false}};
        char out[1024];assert(numeric_feed_encode(out,sizeof(out),"future_device","aabbccddeeff",1,1,fields,3));puts(out);
        assert(!numeric_feed_encode(out,10,"future_device","aabbccddeeff",1,1,fields,3));
        assert(!numeric_feed_encode(out,sizeof(out),"m1","aabbccddeeff",0,1,fields,3));
        fields[0].id="INVALID";assert(!numeric_feed_encode(out,sizeof(out),"m1","aabbccddeeff",1,1,fields,3));
        assert(numeric_feed_encode(out,sizeof(out),"m1","aabbccddeeff",1,2,NULL,0));puts(out);
        assert(!numeric_feed_encode(out,sizeof(out),"m1","aabbccddeeff",1,2,NULL,1));
    }else return 2;
    return 0;
}
'''
        (build / 'test.c').write_text(code)
        cls.binary = build / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I' + str(ROOT), str(build / 'test.c'), '-lm', '-o', str(cls.binary)], check=True)

    def scenario(self, scenario):
        run = subprocess.run([str(self.binary), scenario], check=True, capture_output=True, text=True,
                             timeout=10, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
        rows = [json.loads(line) for line in run.stdout.splitlines()]
        for row in rows:
            self.assertEqual(row['schema'], 2)
            self.assertTrue(row['boot'] and row['sequence'])
            for field in row['sensors'].values():
                self.assertEqual(field['type'], 'number')
                if not field['valid']:
                    self.assertIsNone(field['value'])
        return rows

    def test_physical_fields_have_independent_validity_and_real_age(self):
        rows = self.scenario('fresh')
        s = rows[0]['sensors']
        self.assertAlmostEqual(s['temperature']['value'], -3.5)
        self.assertAlmostEqual(s['formaldehyde']['value'], .127, places=6)
        self.assertAlmostEqual(s['pressure']['value'], 100012.34, places=2)
        self.assertEqual(s['co2']['value'], 750)
        s = rows[1]['sensors']
        self.assertIsNone(s['temperature']['value'])
        self.assertIsNone(s['humidity']['value'])
        self.assertIsNone(s['pm25']['value'])
        self.assertTrue(s['formaldehyde']['valid'])
        self.assertEqual(s['co2']['age_ms'], 5000)
        self.assertTrue(all(not f['valid'] for f in rows[2]['sensors'].values()))

    def test_absent_optional_hardware_is_not_a_zero_measurement(self):
        fields = self.scenario('missing')[0]['sensors']
        self.assertEqual(len(fields), 4)
        self.assertTrue(all(f['value'] is None for f in fields.values()))

    def test_random_failure_is_deferred_and_counters_wrap_without_zero(self):
        self.assertEqual(len(self.scenario('random')), 1)
        self.assertEqual([r['sequence'] for r in self.scenario('wrap')], [0xffffffff, 1])

    def test_publish_errors_propagate_and_attempts_remain_periodic_with_unique_sequences(self):
        self.assertEqual([r['sequence'] for r in self.scenario('publish_error')], [1, 2, 3])

    def test_bounded_encoder_quotes_metadata_and_never_emits_nonfinite_or_invalid_values(self):
        rows = self.scenario('encode')
        self.assertEqual(rows[1]['sensors'], {})
        fields = rows[0]['sensors']
        self.assertEqual(fields['custom_value']['name'], 'Quoted "name" \\ 温度')
        self.assertEqual(fields['custom_value']['value'], 1.25)
        self.assertIsNone(fields['not_available']['value'])
        self.assertIsNone(fields['disabled']['value'])
