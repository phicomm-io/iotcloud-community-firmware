/* Test-only bridge: retain frozen UART fixtures while exercising actual MP
 * commands. This file is never part of a firmware build. */
#include "json.h"
#include "common/sensor_mp.h"
static json_object *fixture_get(json_object *o,const char *key){return o&&json_object_is_type(o,json_type_object)?json_object_object_get(o,key):NULL;}
static void fixture_mp_write(ph_writer *w,const char *key,json_object *o)
{
    if(!o){phw_null(w,key);return;}
    switch(json_object_get_type(o)){
    case json_type_object:{phw_begin(w,key,false);json_object_object_foreach(o,k,v){fixture_mp_write(w,k,v);}phw_end(w);break;}
    case json_type_array:phw_begin(w,key,true);for(int i=0;i<json_object_array_length(o);i++)fixture_mp_write(w,NULL,json_object_array_get_idx(o,i));phw_end(w);break;
    case json_type_boolean:phw_bool(w,key,json_object_get_boolean(o));break;
    case json_type_int:phw_int(w,key,json_object_get_int64(o));break;
    case json_type_double:phw_number(w,key,json_object_get_double(o));break;
    case json_type_string:phw_text_n(w,key,json_object_get_string(o),json_object_get_string_len(o));break;
    default:phw_null(w,key);break;
    }
}
static ph_buf *fixture_mp_view(json_object *o,ph_value *v)
{ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));fixture_mp_write(&w,NULL,o);ph_buf *b=phw_take(&w);if(!b)return NULL;if(!phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,v)){ph_buf_free(b);return NULL;}return b;}
static json_object *fixture_mp_json(ph_value v)
{
    if(v.kind==PHV_MAP){json_object *o=json_object_new_object();ph_iterator it;char key[128];ph_value child;assert(phv_iter(&v,&it));while(phv_next(&it,key,&child))json_object_object_add(o,key,fixture_mp_json(child));return o;}
    if(v.kind==PHV_ARRAY){json_object *o=json_object_new_array();for(unsigned i=0;i<v.count;i++){ph_value child;assert(phv_at(&v,i,&child));json_object_array_add(o,fixture_mp_json(child));}return o;}
    if(v.kind==PHV_TEXT)return json_object_new_string_len((const char *)v.text,v.size);
    if(v.kind==PHV_TRUE||v.kind==PHV_FALSE)return json_object_new_boolean(v.kind==PHV_TRUE);
    if(v.kind==PHV_NULL)return NULL;
    if(v.kind==PHV_REAL)return json_object_new_double(v.real);
    int64_t n;assert(phv_int(&v,&n));return json_object_new_int64(n);
}
static json_object *fixture_mp_roundtrip(json_object *o){ph_value v;ph_buf *b=fixture_mp_view(o,&v);assert(b);json_object *out=fixture_mp_json(v);ph_buf_free(b);return out;}
static json_object *fixture_display_call(const char *action,json_object *p,const char **code)
{
    ph_value v;ph_buf *b=fixture_mp_view(p,&v);if(!b){*code="invalid_params";return NULL;}
    ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));bool handled=!strncmp(action,"co2_",4)?m1_co2_source_mp(action,&v,&w,code):m1_display_mp(action,&v,&w,code);assert(handled);ph_buf_free(b);
    if(strcmp(*code,"accepted")&&strcmp(*code,"ok")){phw_discard(&w);return NULL;}
    ph_buf *out=phw_take(&w);assert(out&&phv_parse(PH_WIRE_MSGPACK,out->buf,out->len,&v));json_object *result=fixture_mp_json(v);ph_buf_free(out);return result;
}
static json_object *m1_display_profile_json(void){const char *code;json_object *p=json_object_new_object();json_object *out=fixture_display_call("display_profile.get",p,&code);json_object_put(p);return out;}
static json_object *m1_display_source_json(void){return m1_display_profile_json();}
static json_object *m1_display_profile_set(json_object *p,const char **code){return fixture_display_call("display_profile.set",p,code);}
static json_object *m1_display_source_set(json_object *p,const char **code){return fixture_display_call("display_source.set",p,code);}
static json_object *m1_display_data_set(json_object *p,const char **code){return fixture_display_call("display_data.set",p,code);}
static json_object *m1_display_value_set(json_object *p,const char **code){return fixture_display_call("display_value.set",p,code);}
static json_object *m1_co2_source_set(json_object *p,const char **code){return fixture_display_call("co2_source.set",p,code);}
static json_object *display_lamps_json(unsigned lamps)
{ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));display_lamps_write(&w,NULL,lamps);ph_buf *b=phw_take(&w);ph_value v;assert(b&&phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&v));json_object *o=fixture_mp_json(v);ph_buf_free(b);return o;}
static bool fixture_lamps_parse(json_object *o,uint16_t *out)
{ph_value v;ph_buf *b=fixture_mp_view(o,&v);if(!b)return false;bool good=display_lamps_parse(v,out);ph_buf_free(b);return good;}
#define display_lamps_parse fixture_lamps_parse
static void fixture_display_sample(const char *id,json_object *o)
{
    /* Legacy schema2 fixtures contain each descriptor inline. Split them at
     * the test boundary; production only sees schema3 MP metadata+snapshot. */
    if(!o||!json_object_is_type(o,json_type_object))return;
    json_object *s=fixture_get(o,"schema");if(!s||!json_object_is_type(s,json_type_int)||json_object_get_int64(s)!=2)return;
    json_object *identity=fixture_get(o,"device_id");if(!identity||!json_object_is_type(identity,json_type_string)||json_object_get_string_len(identity)!=12)return;
    const char *device=json_object_get_string(identity);if(!device||strcmp(device,id))return;
    const char *counters[]={"boot","sequence"};for(unsigned i=0;i<2;i++){json_object *n=fixture_get(o,counters[i]);if(!n||!json_object_is_type(n,json_type_int)||json_object_get_int64(n)<1||json_object_get_int64(n)>UINT32_MAX)return;}
    uint32_t boot=(uint32_t)json_object_get_int64(fixture_get(o,"boot")),seq=(uint32_t)json_object_get_int64(fixture_get(o,"sequence"));
    json_object *sensors=fixture_get(o,"sensors");if(!sensors||!json_object_is_type(sensors,json_type_object))return;
    ph_writer w;ph_value v;assert(phw_init(&w,PH_WIRE_MSGPACK));ph_sensor_metadata_begin(&w,id,boot,1);
    json_object_object_foreach(sensors,key,field){phw_begin(&w,key,false);fixture_mp_write(&w,"name",fixture_get(field,"name"));fixture_mp_write(&w,"type",fixture_get(field,"type"));fixture_mp_write(&w,"unit",fixture_get(field,"unit"));phw_end(&w);}ph_sensor_finish(&w);ph_buf *b=phw_take(&w);if(b&&phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&v))m1_display_source_metadata(id,&v);ph_buf_free(b);
    assert(phw_init(&w,PH_WIRE_MSGPACK));ph_sensor_snapshot_begin(&w,id,boot,seq,1);
    json_object_object_foreach(sensors,key2,field2){phw_begin(&w,key2,false);fixture_mp_write(&w,"value",fixture_get(field2,"value"));fixture_mp_write(&w,"valid",fixture_get(field2,"valid"));fixture_mp_write(&w,"age_ms",fixture_get(field2,"age_ms"));phw_end(&w);}ph_sensor_finish(&w);b=phw_take(&w);if(b&&phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&v))m1_display_source_sample(id,&v);ph_buf_free(b);
}
#define m1_display_source_sample fixture_display_sample
