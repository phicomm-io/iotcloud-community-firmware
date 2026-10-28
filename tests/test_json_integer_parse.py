"""Execute the production integer converter with poisoned errno on host and ARM.

QEMU runs the pinned nano libc ABI, not the MiCO kernel or its network driver.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#define MC_DEBUG(...) ((void)0)
struct json_object { struct { int64_t c_int64; } o; };
struct printbuf { char buf[24]; size_t length; };
static int printbuf_memappend(struct printbuf *pb, const char *text, int length) {
    if (length < 0 || (size_t)length >= sizeof(pb->buf) - pb->length) return -1;
    memcpy(pb->buf + pb->length, text, (size_t)length);
    pb->length += (size_t)length; pb->buf[pb->length] = 0; return 0;
}
#ifdef ARM_TEST
static char arena[128 * 1024];
static size_t used;
void *_sbrk(ptrdiff_t n) {
    if (n < 0 || (size_t)n > sizeof(arena) - used) return (void *)-1;
    void *p = arena + used; used += n; return p;
}
void _exit(int code) {
    register int r0 __asm__("r0") = code;
    register int r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" :: "r"(r0), "r"(r7) : "memory");
    for (;;) {}
}
#endif
'''
VECTORS = r'''
static int verify(void) {
    int64_t out = 0;
    errno = ERANGE;
    if (json_parse_int64("8", &out) || out != 8) return 41;
    const struct { const char *text; int64_t value; } valid[] = {
        {"0",0}, {"-0",0}, {"1",1}, {"7",7}, {"8",8},
        {"+8",8}, {"  -0012\t",-12},
        {"2147483647",INT32_MAX}, {"-2147483648",INT32_MIN},
        {"2147483648",INT64_C(2147483648)}, {"-2147483649",-INT64_C(2147483649)},
        {"4294967295",UINT32_MAX}, {"4294967296",INT64_C(4294967296)},
        {"9223372036854775807",INT64_MAX}, {"-9223372036854775808",INT64_MIN}
    };
    const char *invalid[] = {"", " ", "+", "--1", "1-2", "1x", "1.2", "1 2", "\x80",
        "1e2", "1e+2", "1e-2", "-1e2", "9223372036854775808", "-9223372036854775809",
        "999999999999999999999999999999999999999", "-999999999999999999999999999999999999999"};
    const int poisons[] = {0, ERANGE, EINTR, EAGAIN};
    for (unsigned round = 0; round < 10000; ++round) {
        for (unsigned e = 0; e < sizeof(poisons)/sizeof(poisons[0]); ++e) {
            for (unsigned i = 0; i < sizeof(valid)/sizeof(valid[0]); ++i) {
                errno = poisons[e];
                if (json_parse_int64(valid[i].text, &out) || out != valid[i].value) return 42;
                if (errno != poisons[e]) return 43;
                struct json_object object = { .o.c_int64 = out };
                struct printbuf pb = { {0}, 0 };
                int64_t roundtrip;
                if (json_object_int_to_json_string(&object, &pb) ||
                    json_parse_int64(pb.buf, &roundtrip) || roundtrip != out) return 46;
                if (errno != poisons[e]) return 47;
            }
            for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
                errno = poisons[e]; out = 123;
                if (!json_parse_int64(invalid[i], &out) || out != 123) return 44;
                if (errno != poisons[e]) return 45;
            }
        }
    }
    return 0;
}
#ifdef ARM_TEST
void _start(void) { _exit(verify()); }
#else
int main(void) { return verify(); }
#endif
'''


def production_function():
    sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
    source = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', sdk / 'libraries/utilities/json_c')) / 'json_util.c'
    if not source.exists():
        raise unittest.SkipTest('built pinned SDK required')
    text = source.read_text()
    start = text.index('int json_parse_int64(')
    converter = text[start:text.index('\n#if HAVE_REALLOC', start)]
    text = source.with_name('json_object.c').read_text()
    start = text.index('static int json_object_int_to_json_string')
    return converter + '\n' + text[start:text.index('\n}', start) + 2]


class JsonIntegerParseTest(unittest.TestCase):
    def compile_and_run(self, arm):
        gcc = Path(os.environ.get('ARM_CC', ROOT / 'build/tc1-tools/'
            'gcc-arm-none-eabi-5_4-2016q2/bin/arm-none-eabi-gcc')) if arm else shutil.which('cc')
        qemu = shutil.which('qemu-arm') or shutil.which('qemu-arm-static')
        if not gcc or (arm and (not gcc.exists() or not qemu)):
            self.skipTest('C compiler and, for ARM, pinned toolchain/QEMU required')
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            c, binary = directory / 'test.c', directory / 'test'
            c.write_text(HARNESS + production_function() + VECTORS)
            flags = ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-O1']
            if arm:
                flags += ['-mcpu=cortex-m3', '-mthumb', '-DARM_TEST', '-fno-builtin',
                          '-nostartfiles', '--specs=nano.specs', '--specs=nosys.specs',
                          '-Wl,-e,_start,-Ttext=0x10000']
            else:
                flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
            subprocess.run([str(gcc), *flags, str(c), '-o', str(binary)], check=True)
            subprocess.run(([qemu] if arm else []) + [str(binary)], check=True, timeout=30)

    def test_integer_conversion_ignores_errno_host_sanitizers(self):
        self.compile_and_run(False)

    def test_integer_conversion_ignores_errno_arm_nano(self):
        self.compile_and_run(True)

    def test_json_integer_width_and_float_paths(self):
        from tests.test_stock_runtime import run_host
        run_host(r'''
#include <assert.h>
#include <math.h>
#include <limits.h>
#include "M1/m1.h"
#include "json.h"
uint32_t mico_rtos_get_time(void){return 0;}
int main(void){
 const struct{const char *text;int64_t value;int32_t narrow;} integers[]={
  {"2147483647",INT32_MAX,INT32_MAX},{"2147483648",INT64_C(2147483648),INT32_MAX},
  {"-2147483648",INT32_MIN,INT32_MIN},{"-2147483649",-INT64_C(2147483649),INT32_MIN},
  {"4294967295",UINT32_MAX,INT32_MAX},{"4294967296",INT64_C(4294967296),INT32_MAX},
  {"9223372036854775807",INT64_MAX,INT32_MAX},{"-9223372036854775808",INT64_MIN,INT32_MIN},
  {"0",0,0},{"-12",-12,-12}};
 for(unsigned i=0;i<sizeof(integers)/sizeof(*integers);i++){
  json_object *o=json_tokener_parse(integers[i].text);assert(o&&json_object_is_type(o,json_type_int));
  assert(json_object_get_int64(o)==integers[i].value&&json_object_get_int(o)==integers[i].narrow);
  assert(!strcmp(json_object_to_json_string(o),integers[i].text));json_object_put(o);
  o=json_object_new_int64(integers[i].value);assert(o);
  const char *wire=json_object_to_json_string(o);assert(wire&&!strcmp(wire,integers[i].text));
  json_object *roundtrip=json_tokener_parse(wire);assert(roundtrip&&json_object_is_type(roundtrip,json_type_int));
  assert(json_object_get_int64(roundtrip)==integers[i].value);json_object_put(roundtrip);json_object_put(o);
 }
 const char *invalid[]={"9223372036854775808","-9223372036854775809","99999999999999999999999999999999999"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++)assert(!json_tokener_parse(invalid[i]));
 const struct{const char *text;double value;} floats[]={{"1e2",100},{"1e+2",100},{"1e-2",.01},{"-1e2",-100},{"1.25",1.25}};
 for(unsigned i=0;i<sizeof(floats)/sizeof(*floats);i++){
  json_object *o=json_tokener_parse(floats[i].text);assert(o&&json_object_is_type(o,json_type_double));
  assert(fabs(json_object_get_double(o)-floats[i].value)<1e-9);json_object_put(o);
 }
 return 0;
}
''')

    def test_m1_production_counter_body(self):
        # Counter transport moved from runtime JSON to the actual M1 schema3
        # producer; keep the full unsigned range, not only signed-int examples.
        from tests.test_sensor_producers_mp import SensorProducers
        source = (ROOT/'M1/mqtt.c').read_text()
        start = source.index('static int publish_typed_sensors')
        production = source[start:source.index('\n#include "../common/mqtt/service.h"', start)]
        SensorProducers().compile_run(r'''
#include "common/sensor_mp.h"
#include "common/mqtt_account.h"
#include <assert.h>
#include <limits.h>
#define MQTT_SUCCESS 0
#define PH_MQTT_MORE 1
#define kNoErr 0
static mqtt_account_t account;
static uint32_t sensor_boot,sensor_sequence,sensor_at,sensor_revision=1;
static bool sensor_started,sensor_metadata;static unsigned sensor_mask;
static unsigned calls;static bool retained;static unsigned char bytes[2048];static size_t length;
typedef struct{bool received;uint32_t updated_ms;int temperature_deci_c,humidity_deci_pct,hcho_ug_m3,pm25_ug_m3;}m1_sample_t;
typedef struct{bool received,ready;uint32_t updated_ms;int pressure_centi_pa;}pressure_t;
typedef struct{m1_sample_t sample;pressure_t barometer;struct{bool ready,received;uint32_t updated_ms;int co2_ppm;}scd40;}mqtt_state_t;
static bool m1_sample_fresh(const m1_sample_t *s,uint32_t now){return s->received&&now-s->updated_ms<20000;}
static unsigned m1_sample_fields(const m1_sample_t *s){(void)s;return 15;}
static bool m1_barometer_fresh(const pressure_t *s,uint32_t now){return s->received&&s->ready&&now-s->updated_ms<20000;}
static int MicoRandomNumberRead(uint8_t *out,size_t n){(void)out;(void)n;assert(0);return -1;}
static int publish_bytes(const char *topic,const void *data,size_t n,bool retain){
 assert(strstr(topic,retain?"/metadata":"/state"));assert(n<sizeof(bytes));
 memcpy(bytes,data,n);length=n;retained=retain;calls++;return 0;}
PRODUCTION
static void verify(uint32_t counter,bool metadata){
 ph_value root,number;int64_t n;ph_sensor_view view;
 assert(phv_parse(PH_WIRE_MSGPACK,bytes,length,&root));
 assert(ph_sensor_view_parse(&root,account.mac,metadata,&view));
 assert(view.boot==counter&&view.descriptor_revision==counter);
 assert(phv_get(&root,"boot",&number)&&phv_int(&number,&n)&&n==(int64_t)counter);
 if(!metadata){
  assert(view.sequence==counter&&sensor_sequence==counter);
  assert(phv_get(&root,"sequence",&number)&&phv_int(&number,&n)&&n==(int64_t)counter);
  assert(view.fields.count==4);
  float value;uint32_t age;assert(ph_sensor_reading(&view,"temperature",20000,&value,&age)&&value==25&&age==0);
 }
}
int main(void){
 assert(mqtt_account_init(&account,1,"m1","user","aabbccddeeff"));
 mqtt_state_t sample={0};sample.sample.received=true;sample.sample.updated_ms=1000;sample.sample.temperature_deci_c=250;
 const uint32_t counters[]={1,INT32_MAX,UINT32_C(2147483648),UINT32_MAX};
 for(unsigned i=0;i<sizeof(counters)/sizeof(*counters);i++){
  sensor_boot=sensor_revision=counters[i];sensor_sequence=counters[i]-1;
  sensor_started=sensor_metadata=false;
  unsigned before=calls;assert(!publish_typed_sensors(&sample,1000));assert(retained&&calls==before+1);verify(counters[i],true);
  assert(!publish_typed_sensors(&sample,1000));assert(!retained&&calls==before+2);verify(counters[i],false);
 }
 /* The next full snapshot after UINT32_MAX wraps to positive sequence 1. */
 sensor_started=false;assert(!publish_typed_sensors(&sample,1000));
 ph_value root;ph_sensor_view view;assert(phv_parse(PH_WIRE_MSGPACK,bytes,length,&root));
 assert(ph_sensor_view_parse(&root,account.mac,false,&view)&&view.boot==UINT32_MAX&&view.sequence==1);
 return 0;
}
'''.replace('PRODUCTION', production))


if __name__ == '__main__':
    unittest.main()
