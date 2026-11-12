"""Execute the bounded production encoder and M1 owner publisher, with sanitizers."""
import struct
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def unpack(data):
    offset=0
    def read(n):
        nonlocal offset
        v=data[offset:offset+n];offset+=n
        if len(v)!=n:raise ValueError('truncated MP')
        return v
    def value():
        k=read(1)[0]
        if k<128:return k
        if k>=224:return k-256
        if k in (0xc0,0xc2,0xc3):return {0xc0:None,0xc2:False,0xc3:True}[k]
        if k in (0xca,0xcb):return struct.unpack('>f' if k==0xca else '>d',read(4 if k==0xca else 8))[0]
        if k in (0xcc,0xcd,0xce,0xcf,0xd0,0xd1,0xd2,0xd3):
            return int.from_bytes(read({0xcc:1,0xcd:2,0xce:4,0xcf:8,0xd0:1,0xd1:2,0xd2:4,0xd3:8}[k]),'big',signed=k>=0xd0)
        if 0xa0<=k<=0xbf:return read(k&31).decode('utf-8')
        if k in (0xd9,0xda,0xdb):return read(int.from_bytes(read({0xd9:1,0xda:2,0xdb:4}[k]),'big')).decode('utf-8')
        if 0x80<=k<=0x8f or k in (0xde,0xdf):
            n=(k&15) if k<0x90 else int.from_bytes(read(2 if k==0xde else 4),'big')
            return dict((value(),value()) for _ in range(n))
        raise ValueError('unexpected MP type')
    result=value()
    if offset!=len(data):raise ValueError('trailing MP')
    return result


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
#include <stdio.h>
#include <stdlib.h>
#include "common/sensor_mp.h"
#include "M1/protocol.h"
#include "M1/barometer.h"
#define MQTT_TOPIC_MAX 192
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 17
#define kNoErr 0
static struct {char mac[13];} account={"aabbccddeeff"};
static uint32_t sensor_boot,sensor_sequence,sensor_at,sensor_revision=1;
static unsigned sensor_mask;
static bool sensor_started,sensor_metadata;
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
static void dump(const void *payload,size_t n){const unsigned char*p=payload;for(size_t i=0;i<n;i++)printf("%02x",p[i]);putchar('\n');}
static int publish_bytes(const char *topic,const void *payload,size_t n,bool retained){
    assert(!strcmp(topic,retained?"phicomm/v2/owner/sensor/aabbccddeeff/metadata":"phicomm/v2/owner/sensor/aabbccddeeff/state"));
    dump(payload,n);if(!retained)++published;return publish_result;
}
bool m1_sample_fresh(const m1_sample_t *s,uint32_t now){return s->received&&(uint32_t)(now-s->updated_ms)<M1_STALE_MS;}
''' + state + producer + r'''
int main(int argc,char **argv){
    assert(argc==2);mqtt_state_t s={0};
    s.sample=(m1_sample_t){.received=true,.updated_ms=1000,.temperature_deci_c=-35,
        .humidity_deci_pct=523,.hcho_ug_m3=127,.pm25_ug_m3=28,.climate_status=1,.pm25_status=1};
    s.scd40.ready=s.scd40.received=true;s.scd40.co2_ppm=750;s.scd40.updated_ms=1000;
    s.barometer=(barometer_state_t){.ready=true,.received=true,.pressure_centi_pa=10001234,.updated_ms=1000};
    /* Metadata is retained separately and must succeed before any full state. */
    if(strcmp(argv[1],"random")&&strcmp(argv[1],"encode")){
        assert(!publish_typed_sensors(&s,1000));assert(sensor_metadata&&!published);
    }
    if(!strcmp(argv[1],"fresh")){
        assert(!publish_typed_sensors(&s,1000));assert(published==1);
        assert(!publish_typed_sensors(&s,1001));assert(published==1);
        s.sample.climate_status=0;s.sample.pm25_status=0;
        assert(!publish_typed_sensors(&s,6000));assert(published==2);
        assert(!publish_typed_sensors(&s,26000));assert(published==3);
    }else if(!strcmp(argv[1],"missing")){
        memset(&s,0,sizeof(s));assert(!publish_typed_sensors(&s,0));assert(!published);
        assert(!publish_typed_sensors(&s,1));assert(published==1);
    }else if(!strcmp(argv[1],"random")){
        random_fail=1;publish_typed_sensors(&s,1000);publish_typed_sensors(&s,1001);
        assert(random_calls==1 && !published);random_fail=0;
        publish_typed_sensors(&s,6000);assert(random_calls==2 && sensor_metadata && !published);
        publish_typed_sensors(&s,6000);assert(random_calls==2 && published==1);
    }else if(!strcmp(argv[1],"wrap")){
        sensor_sequence=UINT32_MAX-1;publish_typed_sensors(&s,UINT32_MAX-100);
        publish_typed_sensors(&s,4899);assert(published==2 && sensor_sequence==1);
    }else if(!strcmp(argv[1],"publish_error")){
        publish_result=-7;assert(publish_typed_sensors(&s,1000)==-7);
        assert(sensor_sequence==0 && published==1);
        assert(!publish_typed_sensors(&s,1001) && published==1);
        publish_result=PH_MQTT_MORE;assert(!publish_typed_sensors(&s,6000));
        assert(sensor_sequence==0 && published==2);
        assert(!publish_typed_sensors(&s,6001) && published==2);
        publish_result=0;assert(!publish_typed_sensors(&s,11000));
        assert(sensor_sequence==1 && published==3);
    }else if(!strcmp(argv[1],"encode")){
        char out[1024];ph_writer w;ph_buf buffer;ph_value root;ph_sensor_view view;
        assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,out,sizeof(out)));
        ph_sensor_metadata_begin(&w,"aabbccddeeff",1,1);
        ph_sensor_descriptor_write(&w,"custom_value","Quoted \"name\" \\ 温度","custom");
        ph_sensor_descriptor_write(&w,"not_available","Unavailable","");
        ph_sensor_descriptor_write(&w,"disabled","Disabled","W");ph_sensor_finish(&w);
        size_t n=phw_finish_fixed(&w);assert(n);dump(out,n);
        assert(phv_parse(1,out,n,&root)&&ph_sensor_view_parse(&root,"aabbccddeeff",true,&view));
        assert(phw_init_fixed(&w,1,&buffer,out,sizeof(out)));ph_sensor_snapshot_begin(&w,"aabbccddeeff",1,1,1);
        ph_sensor_reading_write(&w,"custom_value",1.25f,true,17);
        ph_sensor_reading_write(&w,"not_available",NAN,true,0);
        ph_sensor_reading_write(&w,"disabled",123,false,9);ph_sensor_finish(&w);
        n=phw_finish_fixed(&w);assert(n);dump(out,n);
        assert(phw_init_fixed(&w,1,&buffer,out,10));ph_sensor_snapshot_begin(&w,"aabbccddeeff",1,1,1);ph_sensor_finish(&w);assert(!phw_finish_fixed(&w));
        /* Static producer identities need no dynamic validation allocation;
         * malformed envelopes/field IDs are rejected by the common MP view. */
        assert(phw_init_fixed(&w,1,&buffer,out,sizeof(out)));ph_sensor_snapshot_begin(&w,"aabbccddeeff",0,1,1);ph_sensor_finish(&w);
        n=phw_finish_fixed(&w);assert(n&&phv_parse(1,out,n,&root)&&!ph_sensor_view_parse(&root,"aabbccddeeff",false,&view));
        assert(phw_init_fixed(&w,1,&buffer,out,sizeof(out)));ph_sensor_snapshot_begin(&w,"aabbccddeeff",1,1,1);ph_sensor_reading_write(&w,"INVALID",1,true,0);ph_sensor_finish(&w);
        n=phw_finish_fixed(&w);assert(n&&phv_parse(1,out,n,&root)&&!ph_sensor_view_parse(&root,"aabbccddeeff",false,&view));
        assert(phw_init_fixed(&w,1,&buffer,out,sizeof(out)));ph_sensor_snapshot_begin(&w,"aabbccddeeff",1,2,1);ph_sensor_finish(&w);
        n=phw_finish_fixed(&w);assert(n&&phv_parse(1,out,n,&root)&&ph_sensor_view_parse(&root,"aabbccddeeff",false,&view));dump(out,n);
        /* A declared map item with no associated data cannot become a packet. */
        assert(!phv_parse(1,"\x81",1,&root));
    }else return 2;
    return 0;
}
'''
        (build / 'test.c').write_text(code)
        cls.binary = build / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror','-Wno-misleading-indentation',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I' + str(ROOT), str(build / 'test.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'), '-lm', '-o', str(cls.binary)], check=True)

    def scenario(self, scenario):
        run = subprocess.run([str(self.binary), scenario], check=True, capture_output=True, text=True,
                             timeout=10, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
        rows=[];descriptors={}
        for line in run.stdout.splitlines():
            row=unpack(bytes.fromhex(line))
            self.assertEqual(row['schema'],3)
            self.assertTrue(row['boot'] and row['descriptor_revision'])
            if 'fields' in row:
                descriptors=row['fields']
                for field in descriptors.values():self.assertEqual(field['type'],'number')
                continue
            self.assertTrue(row['sequence'])
            row['sensors']={key:{**descriptors.get(key,{}),**value} for key,value in row['sensors'].items()}
            for field in row['sensors'].values():
                if not field['valid']:self.assertIsNone(field['value'])
            rows.append(row)
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

    def test_publish_errors_propagate_and_attempts_remain_periodic_until_success_commits_sequence(self):
        self.assertEqual([r['sequence'] for r in self.scenario('publish_error')], [1, 1, 1])

    def test_bounded_encoder_quotes_metadata_and_never_emits_nonfinite_or_invalid_values(self):
        rows = self.scenario('encode')
        self.assertEqual(rows[1]['sensors'], {})
        fields = rows[0]['sensors']
        self.assertEqual(fields['custom_value']['name'], 'Quoted "name" \\ 温度')
        self.assertEqual(fields['custom_value']['value'], 1.25)
        self.assertIsNone(fields['not_available']['value'])
        self.assertIsNone(fields['disabled']['value'])
