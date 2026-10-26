"""Run actual M1 generic binding/parser/cache code with the pinned SDK JSON."""
import copy
import json
import os
from pathlib import Path
import unittest

from tests import test_m1_display_format
from tests.test_stock_runtime import run_host
from tests.test_http_json_budget import function


class M1GenericFieldsTest(unittest.TestCase):
    def source(self):
        return test_m1_display_format.DisplayFormatTest().source() + r'''
static void dump_profile(const char *leaf){
    const char *directory=getenv("PHICOMM_TEST_GENERIC_FIXTURES");if(!directory)return;
    char path[1024];assert(snprintf(path,sizeof(path),"%s/%s",directory,leaf)<(int)sizeof(path));
    json_object *o=m1_display_profile_json();FILE *f=fopen(path,"w");assert(f);
    assert(fputs(json_object_to_json_string(o),f)>=0&&!fclose(f));json_object_put(o);
}
static json_object *generic_profile(void){
    json_object *p=json_tokener_parse(profile),*a=json_object_object_get(p,"lanes");
    json_object_object_add(p,"revision",json_object_new_int(live.display.revision));
    for(unsigned i=0;i<2;i++){
        json_object *l=json_object_array_get_idx(a,i);
        json_object_object_del(l,"metric");
        json_object_object_add(l,"source",json_object_new_string("mqtt"));
        json_object_object_add(l,"device_id",json_object_new_string("12:34:56:AB:CD:EF"));
        json_object_object_add(l,"field_id",json_object_new_string(i?"other_numeric":"custom_sensor"));
        json_object_object_add(l,"type",json_object_new_string("number"));
        json_object_object_add(l,"unit",json_object_new_string(i?"ticks/interval":"mSv/h"));
    }
    return p;
}
static void generic_begin(void){
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    live.co2=(m1_co2_config_t){.magic=M1_CO2_MAGIC,.interval=5,.low=1000,.high=1500};
    json_object *p=generic_profile();const char *code;
    done(m1_display_profile_set(p,&code));assert(!strcmp(code,"accepted"));json_object_put(p);
}
static json_object *generic_sample(unsigned sequence){
    json_object *p=json_tokener_parse("{\"schema\":2,\"device_id\":\"123456abcdef\",\"model\":\"unlisted_future_model\",\"boot\":42,\"sequence\":1,\"sensors\":{\"custom_sensor\":{\"name\":\"Custom numeric sensor\",\"type\":\"number\",\"unit\":\"mSv/h\",\"value\":1234,\"valid\":true,\"age_ms\":0},\"other_numeric\":{\"name\":\"Another sensor\",\"type\":\"number\",\"unit\":\"ticks/interval\",\"value\":42,\"valid\":true,\"age_ms\":0}}}");assert(p);
    json_object_object_add(p,"sequence",json_object_new_int(sequence));return p;
}
static json_object *generic_field(json_object *p,unsigned slot){return json_object_object_get(json_object_object_get(p,"sensors"),slot?"other_numeric":"custom_sensor");}
static void sample_value(unsigned slot,bool valid,float expected,unsigned reason){
    m1_display_value_t v;m1_display_source_value(&live.display,slot,&v);
    assert(v.valid==valid);if(valid)assert(v.value==expected);else assert(v.reason==reason);
}
'''

    def run_scenario(self, body, prefix=""):
        source = self.source() + prefix + '\nint main(void){\n' + body + '\nassert(!m1_lock&&!config_lock);return 0;}\n'
        if path := os.environ.get('PHICOMM_TEST_GENERIC_C'):
            Path(path).write_text(source)
        run_host(source)

    def test_generic_numeric_snapshot_and_real_get_fixtures(self):
        self.run_scenario(r'''
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    done(v3_call(profile,false,"accepted"));v3_values();
    m1_state.mcu_version_received=m1_state.mcu_extended=true;m1_state.mcu_display_format_schema=3;
    dump_profile("api-raw-profile4.json");
    json_object *p=generic_profile();const char *code;
    done(m1_display_profile_set(p,&code));assert(!strcmp(code,"accepted"));json_object_put(p);
    assert(live.display.revision==2&&live.display.lanes[0].metric==M1_DISPLAY_CUSTOM);
    assert(m1_display_config_valid(&live.display)&&sizeof(live.display)==208);
    const unsigned saved_writes=writes;json_object *feed=generic_sample(1);
    m1_display_source_sample("ffffffffffff",feed);sample_value(0,false,0,M1_DISPLAY_MISSING);
    m1_display_source_sample("123456abcdef",feed);sample_value(0,true,1234,0);sample_value(1,true,42,0);
    dump_profile("mqtt-generic-profile4.json");
    m1_display_value_t v;m1_display_source_value(&live.display,0,&v);assert(v.number==123&&v.format==5&&v.lamps==0x1c0);
    json_object *field=generic_field(feed,0);
    json_object_object_add(field,"value",json_object_new_int(99));
    m1_display_source_sample("123456abcdef",feed);sample_value(0,true,1234,0); /* duplicate */
    json_object_object_add(feed,"sequence",json_object_new_int(2));
    json_object_object_add(field,"type",json_object_new_string("future_type"));
    m1_display_source_sample("123456abcdef",feed);sample_value(0,false,0,M1_DISPLAY_INVALID);sample_value(1,true,42,0);
    json_object *older=generic_sample(1);m1_display_source_sample("123456abcdef",older);json_object_put(older);
    sample_value(0,false,0,M1_DISPLAY_INVALID); /* unknown field advanced replay fence */
    json_object_object_add(feed,"sequence",json_object_new_int(3));json_object_object_del(json_object_object_get(feed,"sensors"),"custom_sensor");
    m1_display_source_sample("123456abcdef",feed);sample_value(0,false,0,M1_DISPLAY_INVALID);sample_value(1,true,42,0);
    json_object_put(feed);feed=generic_sample(4);field=generic_field(feed,0);
    json_object_object_add(field,"valid",json_object_new_boolean(false));json_object_object_add(field,"value",NULL);
    m1_display_source_sample("123456abcdef",feed);sample_value(0,false,0,M1_DISPLAY_INVALID);sample_value(1,true,42,0);
    json_object_object_add(feed,"sequence",json_object_new_int(5));json_object_object_add(field,"valid",json_object_new_boolean(true));
    json_object_object_add(field,"value",json_object_new_double(-2.0));
    m1_display_source_sample("123456abcdef",feed);sample_value(0,false,0,M1_DISPLAY_OUT_OF_RANGE);
    json_object_object_add(feed,"sequence",json_object_new_int(6));json_object_object_add(field,"value",json_object_new_double(0x1.fffffep127));
    m1_display_source_sample("123456abcdef",feed);m1_display_source_value(&live.display,0,&v);assert(v.valid&&v.number==999);
    json_object_object_add(feed,"sequence",json_object_new_int(7));json_object_object_add(field,"value",json_object_new_double(1e39));
    m1_display_source_sample("123456abcdef",feed);sample_value(0,false,0,M1_DISPLAY_INVALID);
    json_object_put(feed);feed=generic_sample(8);m1_display_source_sample("123456abcdef",feed);
    m1_display_source_offline("a_new_model","123456abcdef");sample_value(0,false,0,M1_DISPLAY_OFFLINE);sample_value(1,false,0,M1_DISPLAY_OFFLINE);
    m1_display_source_sample("123456abcdef",feed);sample_value(0,false,0,M1_DISPLAY_OFFLINE); /* duplicate cannot restore */
    json_object_object_add(feed,"sequence",json_object_new_int(9));m1_display_source_sample("123456abcdef",feed);sample_value(0,true,1234,0);
    json_object_object_add(feed,"boot",json_object_new_int(43));json_object_object_add(feed,"sequence",json_object_new_int(1));m1_display_source_sample("123456abcdef",feed);
    json_object_object_add(feed,"boot",json_object_new_int(42));json_object_object_add(feed,"sequence",json_object_new_int(99));
    json_object_object_add(generic_field(feed,0),"value",json_object_new_int(999));m1_display_source_sample("123456abcdef",feed);sample_value(0,true,1234,0);
    now+=4000;sample_value(1,false,0,M1_DISPLAY_STALE);sample_value(0,true,1234,0);
    m1_display_source_mqtt_reset();sample_value(0,false,0,M1_DISPLAY_MISSING);
    assert(writes==saved_writes);json_object_put(feed);
''')

    def test_descriptor_and_profile_boundaries(self):
        bad_profile = []
        good = test_m1_display_format.DisplayFormatTest.profile()
        for lane in good['lanes']:
            lane.pop('metric')
            lane.update(source='mqtt', device_id='123456abcdef', field_id='custom_sensor', type='number', unit='mSv/h')
        for key, values in {
            'field_id': ['', 'A', 'bad/slash', 'bad+', '1field', 'a'*33, '中文'],
            'type': ['', 'future_type', None, 0],
            'unit': ['x'*25, '\n', '\x7f', '中'*9, None, 0],
        }.items():
            for value in values:
                p = copy.deepcopy(good);p['lanes'][0][key] = value;bad_profile.append(p)
        p = copy.deepcopy(good);p['lanes'][0]['metric'] = 'co2';bad_profile.append(p)
        p = copy.deepcopy(good);p['lanes'][0].pop('field_id');bad_profile.append(p)
        cases = ','.join(json.dumps(json.dumps(p)) for p in bad_profile)
        body = r'''
    generic_begin();app_storage_t before=live;const unsigned before_writes=writes;
    const char *bad[]={BAD};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){
        json_object *p=json_tokener_parse(bad[i]);const char *code;
        assert(!m1_display_profile_set(p,&code)&&!strcmp(code,"invalid_params"));json_object_put(p);
        assert(!memcmp(&before,&live,sizeof(live))&&writes==before_writes);
    }
    assert(sensor_field_token("a_9",3,32)&&!sensor_field_token("a-9",3,32));
    assert(sensor_field_text("",0,0,24)&&sensor_field_text("微克/立方米",strlen("微克/立方米"),0,24));
    const char *bad_utf8[]={"\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82","\x80","\xc2x","\x7f"};
    for(unsigned i=0;i<sizeof(bad_utf8)/sizeof(*bad_utf8);i++)assert(!sensor_field_text(bad_utf8[i],strlen(bad_utf8[i]),0,24));
    for(unsigned kind=0;kind<11;kind++){
        json_object *feed=generic_sample(1),*field=generic_field(feed,0);
        if(kind==0)json_object_object_add(field,"name",json_object_new_string(""));
        if(kind==1)json_object_object_add(field,"name",json_object_new_string("bad\nname"));
        if(kind==2)json_object_object_add(field,"name",json_object_new_string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        if(kind==3)json_object_object_add(field,"unit",json_object_new_string("different"));
        if(kind==4)json_object_object_add(field,"valid",json_object_new_int(1));
        if(kind==5)json_object_object_add(field,"value",NULL);
        if(kind==6)json_object_object_add(field,"age_ms",json_object_new_int(20000));
        if(kind==7)json_object_object_add(field,"age_ms",json_object_new_int(-1));
        if(kind==8)json_object_object_add(field,"value",json_object_new_string("1234"));
        if(kind==9)json_object_object_add(field,"type",json_object_new_string("future_type"));
        if(kind==10)json_object_object_add(field,"name",json_object_new_string("\xed\xa0\x80"));
        m1_display_source_reset();m1_display_source_sample("123456abcdef",feed);
        sample_value(0,false,0,M1_DISPLAY_INVALID);sample_value(1,true,42,0);json_object_put(feed);
    }
    /* Descriptor constraints are byte based and don't limit numeric ranges. */
    json_object *p=generic_profile(),*l=json_object_array_get_idx(json_object_object_get(p,"lanes"),0);const char *code;
    json_object_object_add(l,"unit",json_object_new_string(""));json_object_object_add(l,"field_id",json_object_new_string("abcdefghijklmnopqrstuvwxyz012345"));
    done(m1_display_profile_set(p,&code));assert(!strcmp(code,"accepted"));json_object_put(p);
'''.replace('BAD', cases)
        parse = function((test_m1_display_format.test_m1_display_source.ROOT/'common/service.c').read_text(),
                         'json_object *service_parse(')
        nul_cases = []
        for key in ('field_id', 'unit'):
            p = copy.deepcopy(good);p['lanes'][0][key] += '\0suffix'
            nul_cases.append(json.dumps(json.dumps({'action': 'display_profile.set', 'params': p})))
        body += '\nconst char *nul_commands[]={' + ','.join(nul_cases) + '};\n'
        body += 'for(unsigned i=0;i<sizeof(nul_commands)/sizeof(*nul_commands);i++)assert(!service_parse(nul_commands[i],strlen(nul_commands[i])));\n'
        self.run_scenario(body, prefix='\n#include <ctype.h>\n' + parse)

    def test_envelope_limits_and_empty_snapshot_revoke_all_lanes(self):
        self.run_scenario(r'''
    generic_begin();json_object *baseline=generic_sample(1);m1_display_source_sample("123456abcdef",baseline);json_object_put(baseline);
    sample_value(0,true,1234,0);sample_value(1,true,42,0);
    for(unsigned kind=0;kind<9;kind++){
        json_object *feed=generic_sample(2);
        if(kind==0)json_object_object_add(feed,"schema",json_object_new_int(1));
        if(kind==1)json_object_object_add(feed,"schema",json_object_new_boolean(true));
        if(kind==2)json_object_object_del(feed,"device_id");
        if(kind==3)json_object_object_add(feed,"device_id",json_object_new_string("ffffffffffff"));
        if(kind==4)json_object_object_add(feed,"boot",json_object_new_int(0));
        if(kind==5)json_object_object_add(feed,"sequence",json_object_new_int64(UINT64_C(4294967296)));
        if(kind==6)json_object_object_add(feed,"sequence",json_object_new_double(2));
        if(kind==7)json_object_object_add(json_object_object_get(feed,"sensors"),"bad-id",json_object_new_object());
        if(kind==8){
            json_object *sensors=json_object_object_get(feed,"sensors");
            for(unsigned i=0;i<15;i++){char id[16];snprintf(id,sizeof(id),"field_%u",i);json_object_object_add(sensors,id,json_object_new_object());}
        }
        m1_display_source_sample("123456abcdef",feed);json_object_put(feed);
        sample_value(0,true,1234,0);sample_value(1,true,42,0);
    }
    /* Sixteen fields are accepted; unrelated future types don't block the
     * selected numbers, and model is optional display metadata only. */
    json_object *feed=generic_sample(2),*sensors=json_object_object_get(feed,"sensors");
    json_object_object_del(feed,"model");
    for(unsigned i=0;i<14;i++){
        char id[16];snprintf(id,sizeof(id),"field_%u",i);json_object *future=json_object_new_object();
        json_object_object_add(future,"type",json_object_new_string("future"));
        json_object_object_add(future,"value",json_object_new_array());json_object_object_add(sensors,id,future);
    }
    json_object_object_add(generic_field(feed,0),"value",json_object_new_int(77));
    m1_display_source_sample("123456abcdef",feed);sample_value(0,true,77,0);sample_value(1,true,42,0);
    json_object_object_add(feed,"sequence",json_object_new_int(3));json_object_object_add(feed,"sensors",json_object_new_object());
    m1_display_source_sample("123456abcdef",feed);sample_value(0,false,0,M1_DISPLAY_INVALID);sample_value(1,false,0,M1_DISPLAY_INVALID);
    json_object_put(feed);feed=generic_sample(2);m1_display_source_sample("123456abcdef",feed);json_object_put(feed);
    sample_value(0,false,0,M1_DISPLAY_INVALID);sample_value(1,false,0,M1_DISPLAY_INVALID);
    assert(!strcmp(live.display.remote[0].field_id,"custom_sensor")&&!strcmp(live.display.remote[0].unit,"mSv/h"));
''')
