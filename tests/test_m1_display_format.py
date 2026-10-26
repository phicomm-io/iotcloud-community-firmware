"""Exercise production formatted display API, numeric policy and UART owner."""
import copy
import json
import unittest
from tests import test_m1_display_source
from tests.test_http_json_budget import function
from tests.test_stock_runtime import run_host


class DisplayFormatTest(unittest.TestCase):
    @staticmethod
    def profile():
        return {'revision': 0, 'selected_source': 'user', 'double_click': 'switch_display', 'lanes': [
            {'slot': 0, 'source': 'api', 'device_id': '', 'metric': 'co2', 'unit': 'ppm',
             'display': {'mode': 'auto_kilo', 'show_label': False, 'show_unit': True},
             'ttl_seconds': 20, 'color': {'mode': 'thresholds', 'low': 1000, 'high': 1500}},
            {'slot': 1, 'source': 'api', 'device_id': '', 'metric': 'power', 'unit': 'W',
             'display': {'mode': 'integer', 'show_label': True, 'show_unit': False},
             'ttl_seconds': 4, 'color': {'mode': 'fixed', 'lamps': [
                 {'red': True, 'green': False, 'yellow': False},
                 {'red': False, 'green': True, 'yellow': False},
                 {'red': True, 'green': False, 'yellow': True}]}}]}

    def source(self):
        source = test_m1_display_source.DisplaySourceTest()._dual_source().split('int main(void){', 1)[0]
        source += 'static const char *profile=' + json.dumps(json.dumps(self.profile())) + ';\n'
        source += r'''
static bool v3_enabled=true,profile_active=true;
static unsigned bad_query;
static uint8_t v3_profile_status;
static json_object *v3_call(const char *text,bool data,const char *wanted){
    const char *code;json_object *p=json_tokener_parse(text);assert(p);
    json_object *out=data?m1_display_value_set(p,&code):m1_display_profile_set(p,&code);
    assert(!strcmp(code,wanted));json_object_put(p);return out;
}
static void v3_packet(uint8_t p[20]){
    memset(p,0,20);p[0]=0x23;p[1]=0x70;p[2]=sent[2];p[3]=sent[3];p[19]=0x21;
    if(sent[2]==0){p[5]=2;p[6]=0x3c;p[14]=1;p[15]=3;p[16]=1;p[18]=2;}
    if(sent[2]==25)p[4]=v3_profile_status;
    if(sent[2]==26){
        if(!v3_enabled){p[4]=3;return;}
        unsigned slot=sent[4];const m1_display_value_t *v=&dual_sent[slot];
        p[5]=3;p[6]=dual_selected;p[7]=profile_active&&dual_selected&&v->valid;p[8]=slot;
        p[9]=v->number>>8;p[10]=v->number;p[11]=v->format;
        p[12]=v->lamps>>8;p[13]=v->lamps;p[14]=v->ttl;p[15]=profile_active;
        if(bad_query==1)p[15]=0;
        if(bad_query==2)p[13]^=1;
        if(bad_query==3)p[8]^=1;
        if(bad_query==4)p[16]=1;
        if(bad_query==5)p[11]|=8;
        if(bad_query==6)p[12]|=2;
        if(bad_query==7)p[14]=61;
    }
}
static void v3_reply(void){uint8_t p[20];v3_packet(p);assert(m1_mcu_rx(p));}
static void v3_step(unsigned op,unsigned elapsed){
    unsigned before=sends;now+=elapsed;m1_mcu_tick();assert(sends==before+1&&sent[2]==op);
}
static void v3_write(unsigned slot,unsigned number,unsigned format,unsigned lamps,unsigned ttl,const char *wanted){
    json_object *p=json_object_new_object();const char *code;
    json_object_object_add(p,"revision",json_object_new_int(live.display.revision));
    json_object_object_add(p,"slot",json_object_new_int(slot));
    json_object_object_add(p,"ttl_seconds",json_object_new_int(ttl));
    if(ttl){
        json_object_object_add(p,"number",json_object_new_int(number));
        json_object_object_add(p,"decimal",json_object_new_boolean(format&1));
        json_object_object_add(p,"show_label",json_object_new_boolean(format&2));
        json_object_object_add(p,"show_unit",json_object_new_boolean(format&4));
        json_object_object_add(p,"lamps",display_lamps_json(lamps));
    }
    json_object *out=m1_display_value_set(p,&code);assert(!strcmp(code,wanted));
    if(out)json_object_put(out);json_object_put(p);
}
static void v3_values(void){
    v3_write(0,123,5,0x1c0,20,"accepted");
    v3_write(1,999,2,0x115,4,"accepted");
}
static void v3_begin(void){
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    live.co2=(m1_co2_config_t){.magic=M1_CO2_MAGIC,.interval=5,.low=1000,.high=1500};
    done(v3_call(profile,false,"accepted"));v3_values();
    m1_state.uart_ready=m1_state.mcu_version_received=true;
    m1_state.mcu_extended=m1_state.mcu_display_source=true;
    v3_step(0,0);v3_reply();v3_step(26,0);v3_reply();
    assert(m1_state.mcu_display_schema==2&&m1_state.mcu_display_format_schema==(v3_enabled?3:0));
}
static void v3_cache(void){
    v3_step(24,250);assert(sent[10]==0&&sent[4]==0&&sent[5]==123&&sent[6]==5&&
        sent[7]==1&&sent[8]==0xc0&&sent[9]>0);v3_reply();
    v3_step(24,250);assert(sent[10]==1&&sent[4]==3&&sent[5]==0xe7&&sent[6]==2&&
        sent[7]==1&&sent[8]==0x15&&sent[9]>0);v3_reply();
}
static void v3_select(void){v3_step(25,250);assert(sent[4]==1);v3_reply();}
static void v3_query(unsigned slot){v3_step(26,250);assert(sent[4]==slot);v3_reply();}
static void v3_confirmed(bool both,bool left,bool right){
    json_object *o=m1_display_profile_json(),*r=json_object_object_get(o,"runtime");
    assert(json_object_get_int(json_object_object_get(o,"schema"))==3);
    assert(json_object_get_boolean(json_object_object_get(r,"confirmed"))==both);
    json_object *a=json_object_object_get(r,"lanes");
    assert(json_object_get_boolean(json_object_object_get(json_object_array_get_idx(a,0),"confirmed"))==left);
    assert(json_object_get_boolean(json_object_object_get(json_object_array_get_idx(a,1),"confirmed"))==right);
    json_object_put(o);
}
'''
        return source

    def run_scenario(self, body):
        run_host(self.source() + '\nint main(void){\n' + body + '\nassert(!m1_lock&&!config_lock);return 0;}\n')

    def test_missing_null_and_wrong_json_types_are_rejected_without_mutation(self):
        profile = self.profile()
        profile['revision'] = 1
        legacy = copy.deepcopy(profile)
        for lane in legacy['lanes']:
            del lane['display']
            lane.update(decimals=0, scale=1)
            if lane['color']['mode'] == 'fixed':
                lane['color'] = {'mode': 'fixed', 'value': 'green'}
        frame = {'revision': 1, 'slot': 0, 'ttl_seconds': 20, 'number': 0,
                 'decimal': False, 'show_label': False, 'show_unit': False,
                 'lamps': [{'red': False, 'green': False, 'yellow': False} for _ in range(3)]}
        source = {'source': 'local', 'device_id': '', 'double_click': 'none', 'revision': 0}
        old_value = {'revision': 1, 'slot': 0, 'value': 1, 'unit': 'ppm', 'ttl_seconds': 20}
        cases = []

        def variants(original, path=()):
            node = original
            for part in path:
                node = node[part]
            # Number-valued fields permit both JSON number types; other types
            # remain invalid even when false, zero or an empty object is used.
            if isinstance(node, bool):
                replacements = [None, 0, '', [], {}]
            elif isinstance(node, (int, float)):
                replacements = [None, False, '', [], {}]
            elif isinstance(node, str):
                replacements = [None, False, 0, [], {}]
            elif isinstance(node, dict):
                replacements = [None, False, 0, '', []]
            else:
                replacements = [None, False, 0, '', {}]
            for replacement in replacements:
                value = copy.deepcopy(original)
                if path:
                    parent = value
                    for part in path[:-1]:
                        parent = parent[part]
                    parent[path[-1]] = replacement
                else:
                    value = replacement
                yield value
            if isinstance(node, (dict, list)):
                for key in (list(node) if isinstance(node, dict) else range(len(node))):
                    value = copy.deepcopy(original)
                    parent = value
                    for part in path:
                        parent = parent[part]
                    del parent[key]
                    yield value
                    yield from variants(original, (*path, key))

        for index, value in enumerate((profile, frame, source, legacy, old_value)):
            for invalid in variants(value):
                cases.append('{' + str(index) + ',' + json.dumps(json.dumps(invalid)) + '}')
        source = self.source() + '\nstatic const struct {unsigned api;const char *text;} cases[]={\n' + ',\n'.join(cases) + '\n};\n'
        run_host(source + r'''
int main(void){
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    done(v3_call(profile,false,"accepted"));v3_values();
    json_object *(*apis[])(json_object *,const char **)={m1_display_profile_set,m1_display_value_set,
        m1_co2_source_set,m1_display_source_set,m1_display_data_set};
    app_storage_t before=live;display_cache_t cache_before[M1_DISPLAY_LANES];
    memcpy(cache_before,display_cache,sizeof(cache_before));unsigned writes_before=writes;
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++){
        const char *code=NULL;json_object *p=json_tokener_parse(cases[i].text);
        json_object *out=apis[cases[i].api](p,&code);
        assert(!out&&code&&!strcmp(code,"invalid_params"));if(p)json_object_put(p);
        assert(!memcmp(&live,&before,sizeof(live))&&writes==writes_before);
        assert(!memcmp(display_cache,cache_before,sizeof(cache_before)));
    }
    assert(!m1_lock&&!config_lock);return 0;
}
''')

    def test_real_command_ingress_rejects_unicode_and_literal_nul(self):
        parse = function((test_m1_display_source.ROOT/'common/service.c').read_text(),
                         'json_object *service_parse(')
        invalid = []
        for path in (('selected_source',), ('double_click',), ('lanes', 0, 'source'),
                     ('lanes', 0, 'device_id'), ('lanes', 0, 'metric'), ('lanes', 0, 'unit')):
            profile = self.profile()
            node = profile
            for part in path[:-1]:
                node = node[part]
            node[path[-1]] += '\0'
            invalid.append(json.dumps(json.dumps({'action': 'display_profile.set', 'params': profile})))
        source = self.source() + '\n#include <ctype.h>\n' + parse
        source += '\nstatic const char *invalid[]={' + ','.join(invalid) + '};\n'
        run_host(source + r'''
int main(void){
    json_object *p=service_parse(profile,strlen(profile));assert(p);json_object_put(p);
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++)assert(!service_parse(invalid[i],strlen(invalid[i])));
    char literal[]="{\"request\":1}";literal[3]=0;
    assert(!service_parse(literal,sizeof(literal)-1));return 0;
}
''')

    def test_frozen_v2_ids_remain_canonical_while_v3_normalizes(self):
        self.run_scenario(r'''
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    json_object *p=json_tokener_parse(settings),*lane=json_object_array_get_idx(json_object_object_get(p,"lanes"),0);
    json_object_object_add(lane,"source",json_object_new_string("mqtt"));
    const char *bad[]={"123456ABCDEF","12:34:56:ab:cd:ef","12:34:56:AB:CD:EF"},*code;
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){
        json_object_object_add(lane,"device_id",json_object_new_string(bad[i]));
        assert(!m1_display_source_set(p,&code)&&!strcmp(code,"invalid_params")&&writes==0);
    }
    json_object_object_add(lane,"device_id",json_object_new_string("123456abcdef"));
    done(m1_display_source_set(p,&code));assert(!strcmp(code,"accepted")&&writes==1);json_object_put(p);
''')

    def test_malformed_mqtt_samples_do_not_crash_or_create_valid_values(self):
        sample = {'boot': 42, 'sequence': 1, 'model': 'tc1', 'sensors': {
            'power': {'valid': True, 'value': 1234, 'unit': 'W', 'age_ms': 0}}}
        cases = [None, False, 0, '', [], {}]
        for path in (('boot',), ('sequence',), ('model',), ('sensors',), ('sensors', 'power'),
                     ('sensors', 'power', 'valid'), ('sensors', 'power', 'value'),
                     ('sensors', 'power', 'unit'), ('sensors', 'power', 'age_ms')):
            for replacement in (None, [], {}):
                value = copy.deepcopy(sample)
                node = value
                for part in path[:-1]:
                    node = node[part]
                node[path[-1]] = replacement
                cases.append(value)
            value = copy.deepcopy(sample)
            node = value
            for part in path[:-1]:
                node = node[part]
            del node[path[-1]]
            cases.append(value)
        source = self.source() + '\nstatic const char *invalid[]={' + ','.join(
            json.dumps(json.dumps(value)) for value in cases) + '};\n'
        run_host(source + r'''
int main(void){
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    done(v3_call(profile,false,"accepted"));
    live.display.lanes[1].source=M1_DISPLAY_MQTT;strcpy(live.display.lanes[1].device_id,"123456abcdef");
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
        m1_display_source_reset();json_object *p=json_tokener_parse(invalid[i]);
        m1_display_source_sample("123456abcdef",p);m1_display_value_t v;
        m1_display_source_value(&live.display,1,&v);assert(!v.valid&&writes==1);
        if(p)json_object_put(p);
    }
    assert(!m1_lock&&!config_lock);return 0;
}
''')

    def test_v3_get_roundtrips_adjacent_float32_thresholds_and_runtime(self):
        self.run_scenario(r'''
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    const float limits[][2]={{1.0f,0x1.000002p0f},{-0x1.fffffep127f,-0x1.fffffcp127f},
        {0x1.fffffcp127f,0x1.fffffep127f},{0.0f,0x1p-149f},{-0x1p-149f,-0.0f}};
    for(unsigned i=0;i<sizeof(limits)/sizeof(*limits);i++){
        json_object *p=json_tokener_parse(profile),*lane=json_object_array_get_idx(json_object_object_get(p,"lanes"),0);
        json_object *color=json_object_object_get(lane,"color");const char *code;
        json_object_object_add(p,"revision",json_object_new_int(live.display.revision));
        json_object_object_add(color,"low",json_object_new_double(limits[i][0]));
        json_object_object_add(color,"high",json_object_new_double(limits[i][1]));
        done(m1_display_profile_set(p,&code));assert(!strcmp(code,"accepted"));json_object_put(p);
        json_object *o=m1_display_profile_json();const char *wire=json_object_to_json_string(o);assert(wire);
        json_object *read=json_tokener_parse(wire);assert(read);
        color=json_object_object_get(json_object_array_get_idx(json_object_object_get(read,"lanes"),0),"color");
        float low=(float)json_object_get_double(json_object_object_get(color,"low"));
        float high=(float)json_object_get_double(json_object_object_get(color,"high"));
        assert(low==limits[i][0]&&high==limits[i][1]&&low<high);
        json_object_put(read);json_object_put(o);
    }
    v3_values();json_object *o=m1_display_profile_json(),*read=json_tokener_parse(json_object_to_json_string(o));assert(read);
    json_object *lane=json_object_array_get_idx(json_object_object_get(json_object_object_get(read,"runtime"),"lanes"),0);
    assert((float)json_object_get_double(json_object_object_get(lane,"value"))==123/100.0f);
    json_object_put(read);json_object_put(o);
    /* The frozen v2 view keeps its original six-digit wire spelling. */
    live.display.reserved=0;live.display.lanes[0].low=1;live.display.lanes[0].high=0x1.000002p0f;
    o=m1_display_source_json();read=json_tokener_parse(json_object_to_json_string(o));assert(read);
    json_object *color=json_object_object_get(json_object_array_get_idx(json_object_object_get(read,"lanes"),0),"color");
    assert(json_object_get_double(json_object_object_get(color,"low"))==1&&json_object_get_double(json_object_object_get(color,"high"))==1);
    json_object_put(read);json_object_put(o);
''')

    def test_physical_modes_rounding_saturation_and_mask_roundtrip(self):
        self.run_scenario(r'''
    m1_display_lane_config_t lane={.metric=M1_DISPLAY_CUSTOM};
    struct {unsigned mode;float input;unsigned number,decimal;} cases[]={
        {0,0.49f,0,0},{0,0.5f,1,0},{0,998.5f,999,0},{0,1234,999,0},
        {1,999,999,0},{1,999.9f,999,0},{1,1000,100,1},{1,1234,123,1},{1,9999,999,1},{1,65535,999,1},
        {2,0.125f,13,1},{2,1.25f,125,1},{2,9.999f,999,1},{2,999999,999,1},
        {3,1.25f,130,1},{3,9.95f,990,1},{3,999999,990,1},
        {4,12,1,1},{4,1234,123,1},{4,10000,999,1}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++)for(unsigned flags=0;flags<4;flags++){
        m1_display_style_t s={.mode=cases[i].mode,.show_label=flags&1,.show_unit=flags>>1};m1_display_value_t v;
        assert(m1_display_encode_formatted(&lane,&s,0,cases[i].input,20,511,&v));
        assert(v.number==cases[i].number&&v.format==(cases[i].decimal|((flags&1)<<1)|((flags&2)<<1))&&v.lamps==511);
        assert(v.value==cases[i].input&&v.valid);
    }
    for(unsigned mask=0;mask<512;mask++){
        uint16_t read=999;json_object *a=display_lamps_json(mask);assert(display_lamps_parse(a,&read)&&read==mask);json_object_put(a);
    }
    m1_display_style_t s={0};m1_display_value_t v;
    assert(!m1_display_encode_formatted(&lane,&s,0,NAN,20,0,&v));
    assert(!m1_display_encode_formatted(&lane,&s,0,-1,20,0,&v));
    assert(!m1_display_encode_formatted(&lane,&s,0,1,20,512,&v));
    assert(!m1_display_encode_formatted(&lane,&s,0,1,0,0,&v));
    s.mode=M1_DISPLAY_DECIMAL_2;assert(!m1_display_encode_formatted(&lane,&s,1,1,20,0,&v));
''')

    def test_profile_api_mac_strictness_flash_rollback_and_value_lamps(self):
        self.run_scenario(r'''
    m1_display_defaults(&live.display);live.co2_source.magic=M1_CO2_SOURCE_MAGIC;
    app_storage_t before=live;fail_flash=true;assert(!v3_call(profile,false,"storage_error"));assert(!memcmp(&live,&before,sizeof(live)));
    fail_flash=false;done(v3_call(profile,false,"accepted"));assert(live.display.reserved==1&&live.display.style[1].lamps==0x115);
    assert(!v3_call(profile,false,"busy"));v3_values();assert(writes==2);
    m1_display_value_t v;m1_display_source_value(&live.display,0,&v);assert(v.valid&&v.number==123&&v.format==5&&v.lamps==0x1c0);
    m1_display_source_value(&live.display,1,&v);assert(v.valid&&v.number==999&&v.format==2&&v.lamps==0x115);
    v3_write(1,12,0,256,4,"accepted");
    m1_display_source_value(&live.display,1,&v);assert(v.lamps==256&&v.number==12);
    assert(!v3_call("{\"revision\":1,\"slot\":1,\"value\":12,\"unit\":\"W\",\"ttl_seconds\":4,\"lamps\":null}",true,"invalid_params"));
    assert(!v3_call("{\"revision\":1,\"slot\":1,\"ttl_seconds\":0,\"value\":0}",true,"invalid_params"));
    done(v3_call("{\"revision\":1,\"slot\":1,\"ttl_seconds\":0}",true,"accepted"));
    m1_display_source_value(&live.display,1,&v);assert(!v.valid&&writes==2);
    assert(!call("{\"revision\":1,\"slot\":0,\"value\":1,\"unit\":\"ppm\",\"ttl_seconds\":1}",true,"invalid_params"));
    json_object *old=m1_display_source_json(),*a=json_object_object_get(old,"lanes");
    assert(!strcmp(json_object_get_string(json_object_object_get(json_object_array_get_idx(a,0),"source")),"none"));json_object_put(old);
    json_object *p=json_tokener_parse(profile),*lanes=json_object_object_get(p,"lanes"),*l=json_object_array_get_idx(lanes,0);const char *code;
    json_object_object_add(p,"revision",json_object_new_int(1));json_object_object_add(l,"source",json_object_new_string("mqtt"));
    json_object_object_add(l,"device_id",json_object_new_string("12:34:56:AB:CD:EF"));
    done(m1_display_profile_set(p,&code));assert(!strcmp(code,"accepted")&&!strcmp(live.display.lanes[0].device_id,"123456abcdef"));
    json_object_object_add(p,"revision",json_object_new_int(2));json_object_object_add(l,"device_id",json_object_new_string("AA:BB:CC:DD:EE:FF"));
    assert(!m1_display_profile_set(p,&code)&&!strcmp(code,"invalid_params"));
    const char *bad[]={"123456:abcdef","12-34-56-AB-CD-EF","12:34:56:AB:CD:EG"," 123456abcdef","123456abcdef ","12:34:56:ab:cd:ef:00","123456abcde"};char id[13];
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!m1_display_device_id(bad[i],id));
    assert(m1_display_device_id("123456ABCDEF",id)&&!strcmp(id,"123456abcdef"));
    json_object_object_add(l,"device_id",json_object_new_string("123456abcdef"));
    json_object *pm=json_object_array_get_idx(lanes,1),*d=json_object_object_get(pm,"display");
    json_object_object_add(d,"mode",json_object_new_string("decimal_2"));assert(!m1_display_profile_set(p,&code));
    json_object_put(p);
''')

    def test_api_frame_never_scales_rounds_or_uses_threshold_colors(self):
        self.run_scenario(r'''
    v3_begin();m1_display_value_t v;
    for(unsigned mode=0;mode<M1_DISPLAY_MODE_COUNT;mode++){
        live.display.style[0].mode=mode;
        v3_write(0,123,0,0,20,"accepted");m1_display_source_value(&live.display,0,&v);
        assert(v.valid&&v.direct&&v.number==123&&v.format==0&&v.lamps==0&&v.value==123&&v.unit==M1_DISPLAY_UNITLESS);
        v3_write(0,0,7,511,20,"accepted");m1_display_source_value(&live.display,0,&v);
        assert(v.valid&&v.number==0&&v.format==7&&v.lamps==511&&v.unit==M1_DISPLAY_MG_M3);
    }
    for(unsigned mask=0;mask<512;mask++){
        v3_write(0,999,1,mask,20,"accepted");m1_display_source_value(&live.display,0,&v);
        assert(v.number==999&&v.format==1&&v.lamps==mask);
    }
    v3_write(1,999,4,511,4,"accepted");m1_display_source_value(&live.display,1,&v);
    assert(v.number==999&&v.format==4&&v.lamps==511&&v.unit==M1_DISPLAY_UG_M3);
    v3_write(1,123,1,0,4,"invalid_params");v3_write(0,1000,0,0,20,"invalid_params");
    json_object *p=json_tokener_parse("{\"revision\":1,\"slot\":0,\"ttl_seconds\":20,\"number\":1.23,\"decimal\":true,\"show_label\":false,\"show_unit\":false,\"lamps\":[{\"red\":false,\"green\":false,\"yellow\":false},{\"red\":false,\"green\":false,\"yellow\":false},{\"red\":false,\"green\":false,\"yellow\":false}]}");
    const char *code;assert(!m1_display_value_set(p,&code)&&!strcmp(code,"invalid_params"));
    json_object_object_add(p,"number",json_object_new_int(123));json_object_object_add(p,"decimal",json_object_new_int(1));
    assert(!m1_display_value_set(p,&code)&&!strcmp(code,"invalid_params"));json_object_put(p);
    assert(!v3_call("{\"revision\":1,\"slot\":0,\"value\":1234,\"unit\":\"ppm\",\"ttl_seconds\":20}",true,"invalid_params"));
''')

    def test_local_and_typed_sources_still_apply_physical_policy(self):
        self.run_scenario(r'''
    v3_begin();live.display.lanes[0].source=M1_DISPLAY_LOCAL;
    m1_state.scd40.ready=m1_state.scd40.received=true;m1_state.scd40.co2_ppm=1234;m1_state.scd40.updated_ms=now;
    m1_display_value_t v;m1_display_source_value(&live.display,0,&v);
    assert(v.valid&&!v.direct&&v.number==123&&v.format==5&&v.lamps==0x1c0&&v.value==1234);
    live.display.lanes[1].source=M1_DISPLAY_MQTT;strcpy(live.display.lanes[1].device_id,"123456abcdef");
    json_object *sample=json_tokener_parse("{\"boot\":42,\"sequence\":1,\"model\":\"tc1\",\"sensors\":{\"power\":{\"valid\":true,\"value\":1234,\"unit\":\"W\",\"age_ms\":0}}}");
    m1_display_source_sample("123456abcdef",sample);m1_display_source_value(&live.display,1,&v);
    assert(v.valid&&!v.direct&&v.number==999&&v.lamps==0x115&&v.value==1234);
    m1_display_source_offline("tc1","123456abcdef");m1_display_source_value(&live.display,1,&v);assert(!v.valid);
    json_object_put(sample);
''')

    def test_uart_discovery_two_queries_and_production_state(self):
        self.run_scenario(r'''
    v3_begin();v3_confirmed(false,false,false);v3_cache();v3_confirmed(false,false,false);
    v3_select();v3_confirmed(false,false,false);v3_query(0);v3_confirmed(false,true,false);v3_query(1);v3_confirmed(true,true,true);
    const char *path=getenv("PHICOMM_TEST_DISPLAY_V3_JSON");
    if(path){json_object *o=m1_display_profile_json();FILE *f=fopen(path,"w");assert(f);fputs(json_object_to_json_string(o),f);assert(!fclose(f));json_object_put(o);}
    uint8_t button[20]={0x23,0x71,1,2,1};button[19]=0x21;
    assert(m1_button_rx(button)&&manual_display_mode==0);v3_confirmed(false,false,false);
    now=5000;m1_display_value_t v;m1_display_source_value(&live.display,1,&v);assert(!v.valid&&v.reason==M1_DISPLAY_STALE);
    m1_display_source_value(&live.display,0,&v);assert(v.valid);
''')

    def test_unsupported_controller_probe_is_readonly_and_bounded(self):
        self.run_scenario(r'''
    v3_enabled=false;v3_begin();unsigned count=sends;
    for(unsigned i=0;i<35;i++){now+=250;m1_mcu_tick();assert(sends==count);}
    now=format_probe_due;m1_mcu_tick();assert(sends==count+1&&sent[2]==26);v3_reply();
    v3_confirmed(false,false,false);assert(!m1_state.mcu_display_format_schema);
''')

    def test_invalid_or_unselected_query_cannot_confirm(self):
        for bad in range(1, 8):
            with self.subTest(bad=bad):
                self.run_scenario('v3_begin();v3_cache();v3_select();bad_query=' + str(bad) +
                                  ';v3_query(0);v3_confirmed(false,false,false);')

    def test_failed_or_wrong_sequence_profile_ack_cannot_confirm(self):
        self.run_scenario(r'''
    v3_begin();v3_cache();v3_profile_status=2;v3_select();v3_confirmed(false,false,false);
    now=auto_due;v3_query(0);v3_query(1);v3_confirmed(false,false,false);
''')
        self.run_scenario(r'''
    v3_begin();v3_cache();v3_step(25,250);uint8_t p[20];v3_packet(p);p[3]++;m1_mcu_rx(p);
    assert(auto_waiting&&!dual_profile_confirmed);v3_confirmed(false,false,false);
    now=auto_deadline+1;v3_packet(p);m1_mcu_rx(p);assert(!dual_profile_confirmed);v3_confirmed(false,false,false);
''')

    def test_nonempty_formatted_profile_ack_cannot_confirm(self):
        for offset in range(5, 19):
            with self.subTest(offset=offset):
                self.run_scenario(r'''
    v3_begin();v3_cache();v3_step(25,250);uint8_t p[20];v3_packet(p);
    p[''' + str(offset) + r''']=1;assert(m1_mcu_rx(p));
    assert(!dual_profile_confirmed);v3_query(0);v3_query(1);v3_confirmed(false,false,false);
''')

    def test_expired_formatted_reply_cannot_discover_or_confirm(self):
        for late in (0, 1):
            with self.subTest(late_ms=late):
                self.run_scenario(r'''
    m1_display_defaults(&live.display);done(v3_call(profile,false,"accepted"));
    m1_state.uart_ready=m1_state.mcu_version_received=true;
    m1_state.mcu_extended=m1_state.mcu_display_source=true;
    v3_step(0,0);v3_reply();v3_step(26,0);now=auto_deadline+''' + str(late) + r''';v3_reply();
    assert(auto_waiting&&!m1_state.mcu_display_format_schema);
    m1_mcu_tick();assert(!auto_waiting&&!m1_state.mcu_display_format_schema);
    v3_confirmed(false,false,false);
''')
                for slot in (0, 1):
                    with self.subTest(slot=slot):
                        self.run_scenario(r'''
    v3_begin();v3_cache();v3_select();
    ''' + ('v3_query(0);' if slot else '') + r'''
    v3_step(26,250);assert(sent[4]==''' + str(slot) + r''');
    now=auto_deadline+''' + str(late) + r''';v3_reply();
    assert(auto_waiting&&!dual_shown[''' + str(slot) + r'''].value.confirmed);
    m1_mcu_tick();assert(!auto_waiting);
    v3_confirmed(false,''' + ('true' if slot else 'false') + r''',false);
''')
