/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1.h"
#include "../common/json_build.h"
#include "../common/json_number_checked.h"
#include "json_object_private.h"
#include "printbuf.h"
#include <math.h>

/* A bounded cache per configured lane. User display values never enter the
 * physical sensor sample used by MQTT telemetry, linkage or alarm thresholds. */
typedef struct {
    float value;
    uint32_t revision, received_ms, age_ms, lease_ms, boot, sequence, retired_boot[4];
    uint16_t color;
    uint16_t number;
    uint8_t model, format;
    bool valid, received, color_override, formatted, direct, offline;
} display_cache_t;
static display_cache_t display_cache[M1_DISPLAY_LANES];

/* The pinned json-c type check dereferences NULL, and object lookup assumes
 * an object. Validate both before inspecting untrusted nested input. */
static bool display_is_type(json_object *o,enum json_type type)
{return o && json_object_is_type(o,type);}
static json_object *display_get(json_object *o,const char *key)
{return display_is_type(o,json_type_object)?json_object_object_get(o,key):NULL;}

static const char *const metric_names[]={"co2","pm25","formaldehyde","temperature",
    "humidity","pressure","pm10","tvoc","power","energy","fan_speed_percent","custom"};
static const char *const unit_names[]={"ppm","µg/m³","mg/m³","°C","%","Pa","ppb","W","kWh",""};
static const char *const producer_names[]={"","m1","daikin","tc1","a1"};

void m1_display_source_reset(void)
{
    mico_rtos_lock_mutex(&m1_lock);memset(display_cache,0,sizeof(display_cache));mico_rtos_unlock_mutex(&m1_lock);
}
void m1_display_source_mqtt_reset(void)
{
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        m1_display_lane_config_t lane;uint32_t revision;m1_display_lane_get(i,&lane,&revision);
        if(lane.source==M1_DISPLAY_MQTT){mico_rtos_lock_mutex(&m1_lock);memset(&display_cache[i],0,sizeof(display_cache[i]));mico_rtos_unlock_mutex(&m1_lock);}
    }
}
static bool display_counter(json_object *o,const char *key,uint32_t *value)
{
    json_object *v=display_get(o,key);
    if(!display_is_type(v,json_type_int))return false;
    int64_t n=json_object_get_int64(v);
    if(n<1 || (uint64_t)n>UINT32_MAX)return false;
    *value=(uint32_t)n;return true;
}
static bool display_number(json_object *o,float *value)
{
    if(!display_is_type(o,json_type_int)&&!display_is_type(o,json_type_double))return false;
    double n=json_object_get_double(o);
    if(!isfinite(n) || n < -40 || n>999999.0)return false;
    *value=(float)n;return true;
}
static bool display_text(json_object *o,const char *expected)
{
    return display_is_type(o,json_type_string) &&
        (size_t)json_object_get_string_len(o)==strlen(expected) &&
        !strcmp(json_object_get_string(o),expected);
}
static bool producer_metric(unsigned model,unsigned metric)
{
    if(metric==M1_DISPLAY_POWER || metric==M1_DISPLAY_ENERGY)return model==3;
    if(metric==M1_DISPLAY_FAN_SPEED_PERCENT)return model==4;
    if(metric==M1_DISPLAY_PM10 || metric==M1_DISPLAY_TVOC)return model==2;
    return model==1 || model==2;
}
void m1_display_source_offline(const char *model,const char *id)
{
    unsigned producer=0;
    for(unsigned i=1;i<5;i++)if(!strcmp(model,producer_names[i]))producer=i;
    if(!producer)return;
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        m1_display_lane_config_t lane;uint32_t revision;
        if(!m1_display_lane_get(i,&lane,&revision))continue;
        const m1_display_lane_config_t *l=&lane;
        mico_rtos_lock_mutex(&m1_lock);display_cache_t *r=&display_cache[i];
        if(l->source==M1_DISPLAY_MQTT && !strcmp(l->device_id,id) && producer_metric(producer,l->metric) &&
           (!r->model || r->model==producer)){
            r->valid=false;r->offline=true;r->revision=revision;
        }
        mico_rtos_unlock_mutex(&m1_lock);
    }
}
void m1_display_source_sample(const char *id,json_object *body)
{
    uint32_t boot,sequence;
    if(!display_counter(body,"boot",&boot) || !display_counter(body,"sequence",&sequence))return;
    json_object *sensors=display_get(body,"sensors");
    if(!display_is_type(sensors,json_type_object))return;
    json_object *model=display_get(body,"model");unsigned producer=0;
    if(model){
        for(unsigned i=1;i<5;i++)if(display_text(model,producer_names[i]))producer=i;
        if(!producer)return;
    }
    uint32_t now=mico_rtos_get_time();
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        m1_display_lane_config_t lane;uint32_t revision;
        if(!m1_display_lane_get(i,&lane,&revision))continue;
        const m1_display_lane_config_t *l=&lane;
        if(l->source!=M1_DISPLAY_MQTT || strcmp(l->device_id,id))continue;
        if(producer && !producer_metric(producer,l->metric))continue;
        /* Historical air feeds did not carry a model. New actuator/power
         * metrics require an explicit compatible typed producer. */
        if(!producer && l->metric>=M1_DISPLAY_POWER)continue;
        json_object *field=display_get(sensors,metric_names[l->metric]);
        json_object *ok=display_get(field,"valid"),*age=display_get(field,"age_ms");
        float value=0;
        uint32_t lease=(uint32_t)l->ttl_seconds*1000;if(lease>20000)lease=20000;
        bool valid=display_is_type(field,json_type_object) &&
            display_is_type(ok,json_type_boolean) && json_object_get_boolean(ok) &&
            display_text(display_get(field,"unit"),unit_names[l->unit]) &&
            display_is_type(age,json_type_int) && json_object_get_int64(age)>=0 &&
            json_object_get_int64(age)<lease &&
            display_number(display_get(field,"value"),&value) && m1_display_metric_value_valid(l->metric,value);
        mico_rtos_lock_mutex(&m1_lock);
        display_cache_t *r=&display_cache[i];
        bool accept=!(r->revision==revision && r->boot==boot && (int32_t)(sequence-r->sequence)<=0);
        for(unsigned j=0;j<4;j++)if(r->retired_boot[j]==boot)accept=false;
        if(accept){
            if(r->boot && r->boot!=boot){memmove(r->retired_boot+1,r->retired_boot,3*sizeof(*r->retired_boot));r->retired_boot[0]=r->boot;}
            r->revision=revision;r->boot=boot;r->sequence=sequence;r->received_ms=now;
            r->valid=valid;r->value=value;r->age_ms=valid?(uint32_t)json_object_get_int64(age):0;
            r->received=true;r->lease_ms=lease;r->color_override=false;
            r->model=producer;r->offline=false;
        }
        mico_rtos_unlock_mutex(&m1_lock);
    }
}
static bool local_value_locked(const m1_display_lane_config_t *l,uint32_t now,float *value,uint32_t *age,uint32_t *limit)
{
    const m1_sample_t *s=&m1_state.sample;
    *age=now-s->updated_ms;*limit=M1_STALE_MS;
    if(l->metric==M1_DISPLAY_CO2){
        const scd40_state_t *co2=&m1_state.scd40;
        *age=now-co2->updated_ms;*limit=20000;
        *value=co2->co2_ppm;return co2->ready&&co2->received&&co2->co2_ppm;
    }
    if(l->metric==M1_DISPLAY_PRESSURE){
        const barometer_state_t *b=&m1_state.barometer;
        *age=now-b->updated_ms;*limit=M1_BAROMETER_STALE_MS;
        *value=b->pressure_centi_pa/100.0f;return b->ready&&b->received;
    }
    unsigned fields=m1_sample_fields(s);
    if(!s->received)return false;
    if(l->metric==M1_DISPLAY_TEMPERATURE){*value=s->temperature_deci_c/10.0f;return fields&1u;}
    if(l->metric==M1_DISPLAY_HUMIDITY){*value=s->humidity_deci_pct/10.0f;return fields&2u;}
    if(l->metric==M1_DISPLAY_PM25){*value=s->pm25_ug_m3;return fields&4u;}
    if(l->metric==M1_DISPLAY_HCHO){*value=s->hcho_ug_m3/1000.0f;return fields&8u;}
    return false;
}
void m1_display_source_value(const m1_display_config_t *c,unsigned slot,m1_display_value_t *out)
{
    *out=(m1_display_value_t){.reason=M1_DISPLAY_DISABLED};
    if(!c->configured || slot>=M1_DISPLAY_LANES)return;
    const m1_display_lane_config_t *l=&c->lanes[slot];
    if(l->source==M1_DISPLAY_NONE)return;
    out->reason=M1_DISPLAY_MISSING;
    uint32_t now=mico_rtos_get_time(),age=0,limit=(uint32_t)l->ttl_seconds*1000;
    float value=0;uint8_t color=l->color;uint16_t lamps=c->style[slot].lamps;
    bool valid,color_override=false,known=false,direct=false;
    uint16_t direct_number=0;uint8_t direct_format=0;
    mico_rtos_lock_mutex(&m1_lock);
    if(l->source==M1_DISPLAY_LOCAL){
        uint32_t sensor_limit;valid=local_value_locked(l,now,&value,&age,&sensor_limit);
        if(sensor_limit<limit)limit=sensor_limit;
        known=valid;
    }else{
        const display_cache_t *r=&display_cache[slot];
        known=r->revision==c->revision && r->received;
        if(known && r->lease_ms<limit)limit=r->lease_ms;
        uint32_t elapsed=now-r->received_ms;
        valid=r->revision==c->revision && r->valid && r->age_ms<limit && elapsed<limit-r->age_ms;
        if(known){age=r->age_ms+elapsed;value=r->value;}
        if(valid){
            direct=c->reserved==M1_DISPLAY_FORMATTED && l->source==M1_DISPLAY_API && r->direct;
            if(direct){direct_number=r->number;direct_format=r->format;}
            color_override=r->color_override;
            if(color_override){
                if(!r->formatted)color=(uint8_t)r->color;
                lamps=r->formatted?r->color:m1_display_uniform_lamps(r->color);
            }
        }
        if(r->revision==c->revision && r->offline)out->reason=M1_DISPLAY_OFFLINE;
        else if(known && !r->valid)out->reason=M1_DISPLAY_INVALID;
    }
    mico_rtos_unlock_mutex(&m1_lock);
    if(valid && !direct && !m1_display_metric_value_valid(l->metric,value)){valid=false;out->reason=M1_DISPLAY_INVALID;}
    out->age_known=known;out->age_ms=age;
    if(known && age>=limit && out->reason!=M1_DISPLAY_OFFLINE)out->reason=M1_DISPLAY_STALE;
    if(!valid || age>=limit)return;
    if(direct){
        out->value=value;out->number=direct_number;out->format=direct_format;out->lamps=lamps;
        out->ttl=(uint8_t)((limit-age+999)/1000);out->valid=true;out->direct=true;
        out->unit=(direct_format&4)?(slot==0?M1_DISPLAY_MG_M3:M1_DISPLAY_UG_M3):M1_DISPLAY_UNITLESS;
        out->reason=M1_DISPLAY_OK;return;
    }
    if(!color_override && l->color_mode==M1_DISPLAY_COLOR_THRESHOLDS){
        color=value>=l->high?1:value>=l->low?3:2;lamps=m1_display_uniform_lamps(color);
    }
    uint8_t ttl=(uint8_t)((limit-age+999)/1000);
    bool encoded=c->reserved==M1_DISPLAY_FORMATTED?
        m1_display_encode_formatted(l,&c->style[slot],slot,value,ttl,lamps,out):
        m1_display_encode_value(l,slot,value,ttl,color,out);
    out->age_known=true;out->age_ms=age;out->reason=encoded?M1_DISPLAY_OK:M1_DISPLAY_OUT_OF_RANGE;
}

/* Called while the configuration owner holds config_lock. This gives a
 * transient write the same revision boundary as a persistent configuration. */
static void display_data_store(unsigned slot,uint32_t revision,float value,uint8_t ttl,int color,bool formatted)
{
    mico_rtos_lock_mutex(&m1_lock);
    display_cache_t *r=&display_cache[slot];memset(r,0,sizeof(*r));
    r->revision=revision;r->received_ms=mico_rtos_get_time();r->value=value;
    r->valid=ttl!=0;
    r->received=ttl!=0;r->lease_ms=(uint32_t)ttl*1000;
    r->color_override=color>=0;r->color=color>=0?(uint16_t)color:0;r->formatted=formatted;
    mico_rtos_unlock_mutex(&m1_lock);
}
void m1_display_data_store(unsigned slot,uint32_t revision,float value,uint8_t ttl,int color)
{display_data_store(slot,revision,value,ttl,color,false);}
void m1_display_frame_store(unsigned slot,uint32_t revision,unsigned number,unsigned format,unsigned lamps,unsigned ttl)
{
    mico_rtos_lock_mutex(&m1_lock);display_cache_t *r=&display_cache[slot];memset(r,0,sizeof(*r));
    r->revision=revision;r->received_ms=mico_rtos_get_time();r->value=(format&1)?number/100.0f:(float)number;
    r->number=(uint16_t)number;r->format=(uint8_t)format;r->color=(uint16_t)lamps;
    r->valid=r->received=ttl!=0;r->lease_ms=ttl*1000;r->color_override=r->formatted=r->direct=true;
    mico_rtos_unlock_mutex(&m1_lock);
}

static const char *const source_names[]={"local","mqtt","api","none"};
static const char *const color_names[]={"off","red","green","yellow"};
static const char *const reason_names[]={"ok","disabled","missing","stale","offline","invalid","out_of_range","unsupported"};
static const char *const mode_names[]={"integer","auto_kilo","decimal_2","decimal_1","milli"};
static bool display_keys(json_object *o,const char *const *keys,unsigned count)
{
    if(!display_is_type(o,json_type_object))return false;
    json_object_object_foreach(o,key,value){
        (void)value;bool found=false;
        for(unsigned i=0;i<count;i++)if(!strcmp(key,keys[i]))found=true;
        if(!found)return false;
    }
    return true;
}
static bool display_int(json_object *o,const char *key,int low,int high,unsigned *out)
{
    json_object *v=display_get(o,key);
    if(!display_is_type(v,json_type_int))return false;
    int64_t n=json_object_get_int64(v);
    if(n<low || n>high)return false;*out=(unsigned)n;return true;
}
static int display_enum(json_object *o,const char *const *names,unsigned count)
{
    for(unsigned i=0;i<count;i++)if(display_text(o,names[i]))return (int)i;
    return -1;
}
static bool display_float(json_object *o,float *out)
{
    if(!display_is_type(o,json_type_int)&&!display_is_type(o,json_type_double))return false;
    double n=json_object_get_double(o);*out=(float)n;return isfinite(n)&&isfinite(*out);
}
static void display_string(json_object *o,const char *key,const char *value)
{json_build_object_add(o,key,json_object_new_string(value));}
/* Like json_owned.c, this adapter uses the pinned MiCO json-c layout. Only
 * the additive v3 view opts into binary32 round-trip output; v2 is frozen. */
static int display_float32_encode(json_object *o,struct printbuf *pb)
{
    char number[32];
    int n=json_number_format_float32(number,sizeof(number),(float)json_object_get_double(o));
    if(n<0){pb->failed=1;return -1;}
    return printbuf_memappend(pb,number,n);
}
static json_object *display_float32_json(float value)
{
    json_object *o=json_object_new_double(value);
    if(o)o->_to_json_string=display_float32_encode;
    return o;
}
static json_object *display_color_json(const m1_display_lane_config_t *l,bool formatted)
{
    json_object *o=json_object_new_object();
    display_string(o,"mode",l->color_mode==M1_DISPLAY_COLOR_THRESHOLDS?"thresholds":"fixed");
    if(l->color_mode==M1_DISPLAY_COLOR_THRESHOLDS){
        json_build_object_add(o,"low",formatted?display_float32_json(l->low):json_object_new_double(l->low));
        json_build_object_add(o,"high",formatted?display_float32_json(l->high):json_object_new_double(l->high));
    }else display_string(o,"value",color_names[l->color]);
    return o;
}
static json_object *display_lamps_json(unsigned lamps)
{
    json_object *array=json_object_new_array();
    for(unsigned position=0;position<3;position++){
        json_object *o=json_object_new_object();
        json_build_object_add(o,"red",json_object_new_boolean((lamps>>position)&1u));
        json_build_object_add(o,"green",json_object_new_boolean((lamps>>(position+3))&1u));
        json_build_object_add(o,"yellow",json_object_new_boolean((lamps>>(position+6))&1u));
        json_build_array_add(array,o);
    }
    return array;
}
static bool display_lamps_parse(json_object *array,uint16_t *out)
{
    static const char *const keys[]={"red","green","yellow"};
    if(!display_is_type(array,json_type_array) || json_object_array_length(array)!=3)return false;
    unsigned lamps=0;
    for(unsigned position=0;position<3;position++){
        json_object *o=json_object_array_get_idx(array,position);
        if(!display_keys(o,keys,3))return false;
        for(unsigned color=0;color<3;color++){
            json_object *v=display_get(o,keys[color]);
            if(!display_is_type(v,json_type_boolean))return false;
            if(json_object_get_boolean(v))lamps|=1u<<(position+3*color);
        }
    }
    *out=(uint16_t)lamps;return true;
}
static json_object *display_profile_json(bool formatted)
{
    m1_display_config_t c;m1_display_config_get(&c);
    bool active_formatted=c.reserved==M1_DISPLAY_FORMATTED;
    bool configured=c.configured;
    if(formatted && !active_formatted){
        /* A view of old settings never selects the new UART profile. */
        for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
            c.style[i]=m1_display_project_style(&c.lanes[i],i);
            c.lanes[i].scale=1;c.lanes[i].decimals=0;c.lanes[i].color=0;
        }
        c.reserved=M1_DISPLAY_FORMATTED;
    }else if(!formatted && active_formatted){
        /* Frozen v2 clients see a disabled v2 view, never fabricated scalar
         * confirmation of the independently configured formatted profile. */
        uint32_t revision=c.revision;m1_display_defaults(&c);c.revision=revision;
    }
    m1_display_readback_t readback[M1_DISPLAY_LANES];bool selected;
    m1_display_readback(readback,&selected);
    mico_rtos_lock_mutex(&m1_lock);
    bool supported=m1_state.mcu_version_received && m1_state.mcu_extended &&
        (formatted?m1_state.mcu_display_format_schema==3:m1_state.mcu_display_schema==2);
    mico_rtos_unlock_mutex(&m1_lock);
    json_object *o=json_object_new_object(),*lanes=json_object_new_array(),*runtime=json_object_new_object(),*values=json_object_new_array();
    if(formatted)json_build_object_add(o,"schema",json_object_new_int(3));
    json_build_object_add(o,"revision",json_object_new_int(c.revision));
    display_string(o,"selected_source",c.selected?"user":"original");
    display_string(o,"double_click",c.double_click?"switch_display":"none");
    bool matching_profile=formatted==active_formatted && c.configured;
    bool confirmed=supported && matching_profile;
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        const m1_display_lane_config_t *l=&c.lanes[i];
        json_object *lane=json_object_new_object();
        json_build_object_add(lane,"slot",json_object_new_int(i));
        display_string(lane,"source",source_names[l->source]);display_string(lane,"device_id",l->device_id);
        display_string(lane,"metric",metric_names[l->metric]);display_string(lane,"unit",unit_names[l->unit]);
        if(formatted){
            json_object *display=json_object_new_object();
            display_string(display,"mode",mode_names[c.style[i].mode]);
            json_build_object_add(display,"show_label",json_object_new_boolean(c.style[i].show_label));
            json_build_object_add(display,"show_unit",json_object_new_boolean(c.style[i].show_unit));
            json_build_object_add(lane,"display",display);
        }else{
            json_build_object_add(lane,"decimals",json_object_new_int(l->decimals));
            json_build_object_add(lane,"scale",json_object_new_int(l->scale));
        }
        json_build_object_add(lane,"ttl_seconds",json_object_new_int(l->ttl_seconds));
        json_object *color=display_color_json(l,formatted);
        if(formatted && l->color_mode==M1_DISPLAY_COLOR_FIXED){
            json_object_object_del(color,"value");json_build_object_add(color,"lamps",display_lamps_json(c.style[i].lamps));
        }
        json_build_object_add(lane,"color",color);json_build_array_add(lanes,lane);
        m1_display_value_t value;m1_display_source_value(&c,i,&value);
        bool lane_confirmed=matching_profile && readback[i].confirmed && (value.valid?
            readback[i].ttl && readback[i].number==value.number && readback[i].format==value.format &&
            (formatted?readback[i].lamps==value.lamps:readback[i].color==value.color):
            !readback[i].ttl);
        json_object *v=json_object_new_object();
        json_build_object_add(v,"slot",json_object_new_int(i));
        json_build_object_add(v,"valid",json_object_new_boolean(value.valid));
        if(value.valid)json_build_object_add(v,"value",formatted?display_float32_json(value.value):json_object_new_double(value.value));else json_object_object_add(v,"value",NULL);
        display_string(v,"unit",unit_names[value.direct?value.unit:l->unit]);
        if(value.age_known)json_build_object_add(v,"age_ms",json_object_new_int64(value.age_ms));else json_object_object_add(v,"age_ms",NULL);
        json_build_object_add(v,"remaining_ttl_seconds",json_object_new_int(value.valid?value.ttl:0));
        json_build_object_add(v,"effective",json_object_new_boolean(supported && lane_confirmed && readback[i].showing));
        display_string(v,"reason",reason_names[value.reason]);
        if(value.valid){
            json_build_object_add(v,"number",json_object_new_int(value.number));json_build_object_add(v,"format",json_object_new_int(value.format));
            if(formatted)json_build_object_add(v,"lamps",display_lamps_json(value.lamps));
            else json_build_object_add(v,"color",json_object_new_string(color_names[value.color]));
        }else{
            json_object_object_add(v,"number",NULL);json_object_object_add(v,"format",NULL);json_object_object_add(v,formatted?"lamps":"color",NULL);
        }
        json_build_object_add(v,"confirmed",json_object_new_boolean(lane_confirmed));
        confirmed=confirmed && lane_confirmed;json_build_array_add(values,v);
    }
    json_build_object_add(o,"lanes",lanes);
    display_string(runtime,"profile",formatted?(configured?(active_formatted?"formatted":"dual"):"legacy"):c.configured?"dual":"legacy");
    display_string(runtime,"selected_source",selected?"user":"original");
    json_build_object_add(runtime,"mcu_supported",json_object_new_boolean(supported));
    json_build_object_add(runtime,"confirmed",json_object_new_boolean(confirmed));
    display_string(runtime,"confirmation",!supported?"requires_mcu_upgrade":confirmed?"mcu_readback":"pending_mcu_readback");
    json_build_object_add(runtime,"lanes",values);json_build_object_add(o,"runtime",runtime);return o;
}
json_object *m1_display_source_json(void){return display_profile_json(false);}
json_object *m1_display_profile_json(void){return display_profile_json(true);}
static bool display_lane_parse(json_object *o,unsigned slot,m1_display_lane_config_t *l,m1_display_style_t *style,bool formatted)
{
    static const char *const keys[]={"slot","source","device_id","metric","unit","decimals","scale","ttl_seconds","color"};
    static const char *const new_keys[]={"slot","source","device_id","metric","unit","display","ttl_seconds","color"};
    unsigned n;
    if(!display_keys(o,formatted?new_keys:keys,formatted?8:9) || !display_int(o,"slot",slot,slot,&n))return false;
    int source=display_enum(display_get(o,"source"),source_names,4);
    int metric=display_enum(display_get(o,"metric"),metric_names,M1_DISPLAY_METRIC_COUNT);
    int unit=display_enum(display_get(o,"unit"),unit_names,M1_DISPLAY_UNIT_COUNT);
    json_object *id=display_get(o,"device_id");
    if(source<0 || metric<0 || unit<0 || !display_is_type(id,json_type_string) ||
       json_object_get_string_len(id)>17 || (size_t)json_object_get_string_len(id)!=strlen(json_object_get_string(id)))return false;
    *l=(m1_display_lane_config_t){.source=source,.metric=metric,.unit=unit};
    if(formatted){
        if(source==M1_DISPLAY_MQTT){if(!m1_display_device_id(json_object_get_string(id),l->device_id))return false;}
        else if(json_object_get_string_len(id))return false;
    }else{
        /* Frozen v2 accepts only the original canonical ID form. */
        if(json_object_get_string_len(id)>12)return false;
        strcpy(l->device_id,json_object_get_string(id));
    }
    if(formatted){
        static const char *const display_fields[]={"mode","show_label","show_unit"};
        json_object *display=display_get(o,"display"),*label=display_get(display,"show_label"),*unit_flag=display_get(display,"show_unit");
        int mode=display_enum(display_get(display,"mode"),mode_names,M1_DISPLAY_MODE_COUNT);
        if(!display_keys(display,display_fields,3) || mode<0 || (slot==1 && mode!=M1_DISPLAY_INTEGER) ||
           !display_is_type(label,json_type_boolean) || !display_is_type(unit_flag,json_type_boolean))return false;
        *style=(m1_display_style_t){.mode=mode,.show_label=json_object_get_boolean(label),.show_unit=json_object_get_boolean(unit_flag)};
        l->scale=1;
    }else{
        if(!display_int(o,"decimals",0,2,&n))return false;l->decimals=n;
        if(!display_int(o,"scale",1,1000,&n) || (n!=1&&n!=1000))return false;l->scale=n;
    }
    if(!display_int(o,"ttl_seconds",1,60,&n))return false;l->ttl_seconds=n;
    json_object *color=display_get(o,"color");
    if(display_text(display_get(color,"mode"),"fixed")){
        if(formatted){
            static const char *const fixed_keys[]={"mode","lamps"};
            if(!display_keys(color,fixed_keys,2) || !display_lamps_parse(display_get(color,"lamps"),&style->lamps))return false;
        }else{
            static const char *const fixed_keys[]={"mode","value"};
            int c=display_enum(display_get(color,"value"),color_names,4);
            if(!display_keys(color,fixed_keys,2) || c<0)return false;l->color=c;
        }
    }else if(display_text(display_get(color,"mode"),"thresholds")){
        static const char *const threshold_keys[]={"mode","low","high"};
        if(!display_keys(color,threshold_keys,3) || !display_float(display_get(color,"low"),&l->low) ||
           !display_float(display_get(color,"high"),&l->high) || l->low>=l->high)return false;
        l->color_mode=M1_DISPLAY_COLOR_THRESHOLDS;
    }else return false;
    return true;
}
static json_object *display_config_set(json_object *p,const char **code,bool formatted)
{
    static const char *const keys[]={"revision","selected_source","double_click","lanes"};
    static const char *const selected[]={"original","user"},*const button[]={"none","switch_display"};
    *code="invalid_params";unsigned revision;
    if(!display_keys(p,keys,4) || !display_int(p,"revision",0,INT32_MAX,&revision))return NULL;
    int choice=display_enum(display_get(p,"selected_source"),selected,2);
    int action=display_enum(display_get(p,"double_click"),button,2);
    json_object *lanes=display_get(p,"lanes");
    if(choice<0 || action<0 || !display_is_type(lanes,json_type_array) || json_object_array_length(lanes)!=M1_DISPLAY_LANES)return NULL;
    m1_display_config_t c={.magic=M1_DISPLAY_MAGIC,.revision=revision,.configured=1,.selected=choice,.double_click=action,.reserved=formatted?M1_DISPLAY_FORMATTED:0};
    bool used[M1_DISPLAY_LANES]={false};
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        json_object *lane=json_object_array_get_idx(lanes,i);unsigned slot;
        if(!display_int(lane,"slot",0,M1_DISPLAY_LANES-1,&slot) || used[slot] || !display_lane_parse(lane,slot,&c.lanes[slot],&c.style[slot],formatted))return NULL;
        used[slot]=true;
    }
    *code=m1_display_config_save(&c);return !strcmp(*code,"accepted")?display_profile_json(formatted):NULL;
}
json_object *m1_display_source_set(json_object *p,const char **code){return display_config_set(p,code,false);}
json_object *m1_display_profile_set(json_object *p,const char **code){return display_config_set(p,code,true);}
json_object *m1_display_data_set(json_object *p,const char **code)
{
    static const char *const keys[]={"revision","slot","value","unit","ttl_seconds","color"};
    *code="invalid_params";unsigned revision,slot,ttl;
    if(!display_keys(p,keys,6) || !display_int(p,"revision",0,INT32_MAX,&revision) ||
       !display_int(p,"slot",0,M1_DISPLAY_LANES-1,&slot) || !display_int(p,"ttl_seconds",0,60,&ttl))return NULL;
    float value=0;int unit=0,color=-1;
    if(!ttl){
        static const char *const clear_keys[]={"revision","slot","ttl_seconds"};
        if(!display_keys(p,clear_keys,3))return NULL;
        m1_display_lane_config_t lane;uint32_t current;m1_display_lane_get(slot,&lane,&current);unit=lane.unit;
    }else{
        unit=display_enum(display_get(p,"unit"),unit_names,M1_DISPLAY_UNIT_COUNT);
        if(unit<0 || !display_number(display_get(p,"value"),&value))return NULL;
        json_object *v=display_get(p,"color");
        if(v){color=display_enum(v,color_names,4);if(color<0)return NULL;}
    }
    *code=m1_display_data_save(slot,revision,value,unit,ttl,color);
    return !strcmp(*code,"accepted")?m1_display_source_json():NULL;
}
json_object *m1_display_value_set(json_object *p,const char **code)
{
    static const char *const keys[]={"revision","slot","number","decimal","show_label","show_unit","ttl_seconds","lamps"};
    static const char *const clear_keys[]={"revision","slot","ttl_seconds"};
    *code="invalid_params";unsigned revision,slot,ttl,number=0,format=0;uint16_t lamps=0;
    if(!display_keys(p,keys,8) || !display_int(p,"revision",0,INT32_MAX,&revision) ||
       !display_int(p,"slot",0,M1_DISPLAY_LANES-1,&slot) || !display_int(p,"ttl_seconds",0,60,&ttl))return NULL;
    if(!ttl){
        if(!display_keys(p,clear_keys,3))return NULL;
    }else{
        static const char *const flags[]={"decimal","show_label","show_unit"};
        if(!display_int(p,"number",0,999,&number) || !display_lamps_parse(display_get(p,"lamps"),&lamps))return NULL;
        for(unsigned i=0;i<3;i++){
            json_object *v=display_get(p,flags[i]);
            if(!display_is_type(v,json_type_boolean))return NULL;
            if(json_object_get_boolean(v))format|=1u<<i;
        }
        if(slot==1 && (format&1u))return NULL;
    }
    *code=m1_display_frame_save(slot,revision,number,format,lamps,ttl);
    return !strcmp(*code,"accepted")?m1_display_profile_json():NULL;
}
