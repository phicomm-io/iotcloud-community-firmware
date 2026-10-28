/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1.h"
#include "../common/typed_wire.h"
#include "../common/sensor_mp.h"
#include <math.h>

/* A bounded cache per configured lane. User display values never enter the
 * physical sensor sample used by MQTT telemetry, linkage or alarm thresholds. */
typedef struct {
    float value;
    uint32_t revision, received_ms, age_ms, lease_ms, boot, sequence, retired_boot[4];
    uint32_t metadata_boot, descriptor_revision;
    bool descriptor_valid;
    uint16_t color;
    uint16_t number;
    uint8_t format;
    bool valid, received, color_override, formatted, direct, offline;
} display_cache_t;
static display_cache_t display_cache[M1_DISPLAY_LANES];

/* Views only borrow the request for the duration of a synchronous call. */
static ph_value display_get(ph_value o,const char *key)
{ph_value v={.kind=PHV_MISSING};phv_get(&o,key,&v);return v;}
static ph_value display_at(ph_value o,unsigned i)
{ph_value v={.kind=PHV_MISSING};phv_at(&o,i,&v);return v;}
static bool display_text(ph_value o,const char *s){return phv_equal_text(&o,s);}
static bool display_float(ph_value o,float *out)
{double n;if(!phv_number(&o,&n))return false;*out=(float)n;return isfinite(n)&&isfinite(*out);}
#define display_number display_float
static bool display_int(ph_value o,const char *key,int low,int high,unsigned *out)
{ph_value v=display_get(o,key);int64_t n;if(!phv_int(&v,&n)||n<low||n>high)return false;*out=(unsigned)n;return true;}
static bool display_keys(ph_value o,const char *const *keys,unsigned count)
{if(o.kind!=PHV_MAP)return false;ph_iterator it;ph_value v;char key[128];phv_iter(&o,&it);while(phv_next(&it,key,&v)){bool found=false;for(unsigned i=0;i<count;i++)if(!strcmp(key,keys[i]))found=true;if(!found)return false;}return true;}
static int display_enum(ph_value o,const char *const *names,unsigned count)
{for(unsigned i=0;i<count;i++)if(display_text(o,names[i]))return (int)i;return -1;}
#define metric_names m1_display_metric_names
#define unit_names m1_display_unit_names

void m1_display_source_reset(void)
{
    mico_rtos_lock_mutex(&m1_lock);memset(display_cache,0,sizeof(display_cache));mico_rtos_unlock_mutex(&m1_lock);
}
void m1_display_source_mqtt_reset(void)
{
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        m1_display_lane_config_t lane;uint32_t revision;m1_display_lane_get(i,&lane,NULL,&revision);
        if(lane.source==M1_DISPLAY_MQTT){mico_rtos_lock_mutex(&m1_lock);memset(&display_cache[i],0,sizeof(display_cache[i]));mico_rtos_unlock_mutex(&m1_lock);}
    }
}
void m1_display_source_offline(const char *model,const char *id)
{
    (void)model; /* Availability identifies a device; models do not define fields. */
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        m1_display_lane_config_t lane;uint32_t revision;
        if(!m1_display_lane_get(i,&lane,NULL,&revision))continue;
        if(lane.source!=M1_DISPLAY_MQTT || strcmp(lane.device_id,id))continue;
        mico_rtos_lock_mutex(&m1_lock);display_cache_t *r=&display_cache[i];
        r->valid=false;r->offline=true;r->revision=revision;
        mico_rtos_unlock_mutex(&m1_lock);
    }
}
void m1_display_source_metadata(const char *id,const ph_value *body)
{
    ph_sensor_view view;if(!ph_sensor_view_parse(body,id,true,&view))return;
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        m1_display_lane_config_t lane;m1_display_remote_t remote;uint32_t revision;
        if(!m1_display_lane_get(i,&lane,&remote,&revision)||lane.source!=M1_DISPLAY_MQTT||strcmp(lane.device_id,id))continue;
        bool valid=ph_sensor_descriptor_matches(&view,remote.field_id,remote.unit);
        mico_rtos_lock_mutex(&m1_lock);display_cache_t *r=&display_cache[i];
        bool accept=true;for(unsigned j=0;j<4;j++)if(r->retired_boot[j]==view.boot)accept=false;
        if(r->revision==revision && r->metadata_boot==view.boot && (int32_t)(view.descriptor_revision-r->descriptor_revision)<0)accept=false;
        if(accept){
            if(r->metadata_boot && r->metadata_boot!=view.boot){memmove(r->retired_boot+1,r->retired_boot,3*sizeof(*r->retired_boot));r->retired_boot[0]=r->metadata_boot;}
            if(r->revision!=revision||r->metadata_boot!=view.boot||r->descriptor_revision!=view.descriptor_revision||!valid)r->valid=false;
            r->revision=revision;r->metadata_boot=view.boot;r->descriptor_revision=view.descriptor_revision;r->descriptor_valid=valid;
        }
        mico_rtos_unlock_mutex(&m1_lock);
    }
}
void m1_display_source_sample(const char *id,const ph_value *body)
{
    ph_sensor_view view;if(!ph_sensor_view_parse(body,id,false,&view))return;
    uint32_t now=mico_rtos_get_time();
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        m1_display_lane_config_t lane;m1_display_remote_t remote;uint32_t revision;
        if(!m1_display_lane_get(i,&lane,&remote,&revision)||lane.source!=M1_DISPLAY_MQTT||strcmp(lane.device_id,id))continue;
        uint32_t lease=(uint32_t)lane.ttl_seconds*1000;if(lease>20000)lease=20000;
        float value=0;uint32_t age=0;bool valid=ph_sensor_reading(&view,remote.field_id,lease,&value,&age);
        mico_rtos_lock_mutex(&m1_lock);display_cache_t *r=&display_cache[i];
        bool accept=!(r->revision==revision && r->boot==view.boot && (int32_t)(view.sequence-r->sequence)<=0);
        for(unsigned j=0;j<4;j++)if(r->retired_boot[j]==view.boot)accept=false;
        if(accept){
            bool matches=r->revision==revision&&r->metadata_boot==view.boot&&r->descriptor_revision==view.descriptor_revision&&r->descriptor_valid;
            /* Unknown descriptors revoke, but never renew the previous lease. */
            if(!matches)r->valid=false;
            else{
                r->revision=revision;r->boot=view.boot;r->sequence=view.sequence;r->received_ms=now;
                r->valid=valid;r->value=value;r->age_ms=valid?age:0;r->received=true;r->lease_ms=lease;r->color_override=false;r->offline=false;
            }
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
    if(valid && !direct && l->source!=M1_DISPLAY_MQTT && !m1_display_metric_value_valid(l->metric,value)){valid=false;out->reason=M1_DISPLAY_INVALID;}
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
static void display_lamps_write(ph_writer *w,const char *key,unsigned lamps)
{phw_begin(w,key,true);for(unsigned position=0;position<3;position++){phw_begin(w,NULL,false);phw_bool(w,"red",(lamps>>position)&1u);phw_bool(w,"green",(lamps>>(position+3))&1u);phw_bool(w,"yellow",(lamps>>(position+6))&1u);phw_end(w);}phw_end(w);}
static bool display_lamps_parse(ph_value array,uint16_t *out)
{
    static const char *const keys[]={"red","green","yellow"};
    if(array.kind!=PHV_ARRAY || array.count!=3)return false;
    unsigned lamps=0;
    for(unsigned position=0;position<3;position++){
        ph_value o=display_at(array,position);
        if(!display_keys(o,keys,3))return false;
        for(unsigned color=0;color<3;color++){
            ph_value v=display_get(o,keys[color]);
            if((v.kind!=PHV_TRUE&&v.kind!=PHV_FALSE))return false;
            if(v.kind==PHV_TRUE)lamps|=1u<<(position+3*color);
        }
    }
    *out=(uint16_t)lamps;return true;
}
void m1_display_profile_write(ph_writer *w)
{
    m1_display_config_t c;m1_display_config_get(&c);
    bool active_formatted=c.reserved==M1_DISPLAY_FORMATTED,configured=c.configured;
    if(!active_formatted){for(unsigned i=0;i<M1_DISPLAY_LANES;i++){c.style[i]=m1_display_project_style(&c.lanes[i],i);c.lanes[i].scale=1;c.lanes[i].decimals=0;c.lanes[i].color=0;}c.reserved=M1_DISPLAY_FORMATTED;}
    m1_display_readback_t readback[M1_DISPLAY_LANES];bool selected;m1_display_readback(readback,&selected);
    mico_rtos_lock_mutex(&m1_lock);bool supported=m1_state.mcu_version_received&&m1_state.mcu_extended&&m1_state.mcu_display_format_schema==3;mico_rtos_unlock_mutex(&m1_lock);
    bool matching_profile=active_formatted&&configured,confirmed=supported&&matching_profile;
    m1_display_value_t values[M1_DISPLAY_LANES];bool lane_confirmed[M1_DISPLAY_LANES];
    phw_begin(w,NULL,false);phw_u32(w,"schema",4);phw_u32(w,"revision",c.revision);
    phw_text(w,"selected_source",c.selected?"user":"original");phw_text(w,"double_click",c.double_click?"switch_display":"none");
    phw_begin(w,"lanes",true);
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        const m1_display_lane_config_t *l=&c.lanes[i];const char *unit=l->source==M1_DISPLAY_MQTT?c.remote[i].unit:unit_names[l->unit];
        phw_begin(w,NULL,false);phw_u32(w,"slot",i);phw_text(w,"source",source_names[l->source]);phw_text(w,"device_id",l->device_id);
        if(l->source==M1_DISPLAY_MQTT){phw_text(w,"field_id",c.remote[i].field_id);phw_text(w,"type","number");}else phw_text(w,"metric",metric_names[l->metric]);
        phw_text(w,"unit",unit);phw_begin(w,"display",false);phw_text(w,"mode",mode_names[c.style[i].mode]);phw_bool(w,"show_label",c.style[i].show_label);phw_bool(w,"show_unit",c.style[i].show_unit);phw_end(w);
        phw_u32(w,"ttl_seconds",l->ttl_seconds);phw_begin(w,"color",false);phw_text(w,"mode",l->color_mode==M1_DISPLAY_COLOR_THRESHOLDS?"thresholds":"fixed");
        if(l->color_mode==M1_DISPLAY_COLOR_THRESHOLDS){phw_number(w,"low",l->low);phw_number(w,"high",l->high);}else display_lamps_write(w,"lamps",c.style[i].lamps);
        phw_end(w);phw_end(w);
        m1_display_source_value(&c,i,&values[i]);const m1_display_value_t *v=&values[i];
        lane_confirmed[i]=matching_profile&&readback[i].confirmed&&(v->valid?readback[i].ttl&&readback[i].number==v->number&&readback[i].format==v->format&&readback[i].lamps==v->lamps:!readback[i].ttl);
        confirmed=confirmed&&lane_confirmed[i];
    }phw_end(w);
    phw_begin(w,"runtime",false);phw_text(w,"profile",configured?(active_formatted?"formatted":"dual"):"legacy");phw_text(w,"selected_source",selected?"user":"original");phw_bool(w,"mcu_supported",supported);phw_bool(w,"confirmed",confirmed);phw_text(w,"confirmation",!supported?"requires_mcu_upgrade":confirmed?"mcu_readback":"pending_mcu_readback");
    phw_begin(w,"lanes",true);
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        const m1_display_value_t *v=&values[i];const m1_display_lane_config_t *l=&c.lanes[i];const char *unit=l->source==M1_DISPLAY_MQTT?c.remote[i].unit:unit_names[l->unit];
        phw_begin(w,NULL,false);phw_u32(w,"slot",i);phw_bool(w,"valid",v->valid);if(v->valid)phw_number(w,"value",v->value);else phw_null(w,"value");phw_text(w,"unit",v->direct?unit_names[v->unit]:unit);
        if(v->age_known)phw_u32(w,"age_ms",v->age_ms);else phw_null(w,"age_ms");phw_u32(w,"remaining_ttl_seconds",v->valid?v->ttl:0);phw_bool(w,"effective",supported&&lane_confirmed[i]&&readback[i].showing);phw_text(w,"reason",reason_names[v->reason]);
        if(v->valid){phw_u32(w,"number",v->number);phw_u32(w,"format",v->format);display_lamps_write(w,"lamps",v->lamps);}else{phw_null(w,"number");phw_null(w,"format");phw_null(w,"lamps");}
        phw_bool(w,"confirmed",lane_confirmed[i]);phw_end(w);
    }phw_end(w);phw_end(w);phw_end(w);
}
static bool display_binding_string(ph_value o,char *out,bool field)
{
    if(o.kind!=PHV_TEXT)return false;
    const char *text=(const char *)o.text;size_t n=o.size;
    if(field?!sensor_field_token(text,n,SENSOR_FIELD_ID_MAX):!sensor_field_text(text,n,0,SENSOR_FIELD_UNIT_MAX))return false;
    memcpy(out,text,n);out[n]=0;return true;
}
static bool display_lane_parse(ph_value o,unsigned slot,m1_display_lane_config_t *l,
                               m1_display_style_t *style,m1_display_remote_t *remote)
{
    static const char *const local_keys[]={"slot","source","device_id","metric","unit","display","ttl_seconds","color"};
    static const char *const mqtt_keys[]={"slot","source","device_id","field_id","type","unit","display","ttl_seconds","color"};
    unsigned n;int source=display_enum(display_get(o,"source"),source_names,4);
    if(source<0 || !display_keys(o,source==M1_DISPLAY_MQTT?mqtt_keys:local_keys,source==M1_DISPLAY_MQTT?9:8) ||
       !display_int(o,"slot",slot,slot,&n))return false;
    ph_value id=display_get(o,"device_id");char device_id[18];
    if(id.kind!=PHV_TEXT || id.size>17)return false;
    *l=(m1_display_lane_config_t){.source=source,.metric=M1_DISPLAY_CUSTOM,.unit=M1_DISPLAY_UNITLESS,.scale=1};
    if(source==M1_DISPLAY_MQTT){
        if(!phv_text(&id,device_id,sizeof(device_id)) || !m1_display_device_id(device_id,l->device_id) || !display_text(display_get(o,"type"),"number") ||
           !display_binding_string(display_get(o,"field_id"),remote->field_id,true) ||
           !display_binding_string(display_get(o,"unit"),remote->unit,false))return false;
    }else{
        int metric=display_enum(display_get(o,"metric"),metric_names,M1_DISPLAY_METRIC_COUNT);
        int unit=display_enum(display_get(o,"unit"),unit_names,M1_DISPLAY_UNIT_COUNT);
        if(id.size || metric<0 || unit<0)return false;
        l->metric=metric;l->unit=unit;
    }
    static const char *const display_fields[]={"mode","show_label","show_unit"};
    ph_value display=display_get(o,"display"),label=display_get(display,"show_label"),unit_flag=display_get(display,"show_unit");
    int mode=display_enum(display_get(display,"mode"),mode_names,M1_DISPLAY_MODE_COUNT);
    if(!display_keys(display,display_fields,3) || mode<0 || (slot==1 && mode!=M1_DISPLAY_INTEGER) ||
       (label.kind!=PHV_TRUE&&label.kind!=PHV_FALSE) || (unit_flag.kind!=PHV_TRUE&&unit_flag.kind!=PHV_FALSE))return false;
    *style=(m1_display_style_t){.mode=mode,.show_label=label.kind==PHV_TRUE,.show_unit=unit_flag.kind==PHV_TRUE};
    if(!display_int(o,"ttl_seconds",1,60,&n))return false;l->ttl_seconds=n;
    ph_value color=display_get(o,"color");
    if(display_text(display_get(color,"mode"),"fixed")){
        static const char *const fixed_keys[]={"mode","lamps"};
        if(!display_keys(color,fixed_keys,2) || !display_lamps_parse(display_get(color,"lamps"),&style->lamps))return false;
    }else if(display_text(display_get(color,"mode"),"thresholds")){
        static const char *const threshold_keys[]={"mode","low","high"};
        if(!display_keys(color,threshold_keys,3) || !display_float(display_get(color,"low"),&l->low) ||
           !display_float(display_get(color,"high"),&l->high) || l->low>=l->high)return false;
        l->color_mode=M1_DISPLAY_COLOR_THRESHOLDS;
    }else return false;
    return true;
}
static bool display_profile_set(ph_value p,const char **code)
{
    static const char *const keys[]={"revision","selected_source","double_click","lanes"};
    static const char *const selected[]={"original","user"},*const button[]={"none","switch_display"};
    *code="invalid_params";unsigned revision;
    if(!display_keys(p,keys,4) || !display_int(p,"revision",0,INT32_MAX,&revision))return false;
    int choice=display_enum(display_get(p,"selected_source"),selected,2);
    int action=display_enum(display_get(p,"double_click"),button,2);
    ph_value lanes=display_get(p,"lanes");
    if(choice<0 || action<0 || lanes.kind!=PHV_ARRAY || lanes.count!=M1_DISPLAY_LANES)return false;
    m1_display_config_t c={.magic=M1_DISPLAY_MAGIC,.revision=revision,.configured=1,.selected=choice,.double_click=action,.reserved=M1_DISPLAY_FORMATTED};
    bool used[M1_DISPLAY_LANES]={false};
    for(unsigned i=0;i<M1_DISPLAY_LANES;i++){
        ph_value lane=display_at(lanes,i);unsigned slot;
        if(!display_int(lane,"slot",0,M1_DISPLAY_LANES-1,&slot) || used[slot] ||
           !display_lane_parse(lane,slot,&c.lanes[slot],&c.style[slot],&c.remote[slot]))return false;
        used[slot]=true;
    }
    *code=m1_display_config_save(&c);return !strcmp(*code,"accepted");
}

static bool display_data_set(ph_value p,const char **code)
{
    static const char *const keys[]={"revision","slot","value","unit","ttl_seconds","color"};
    *code="invalid_params";unsigned revision,slot,ttl;
    if(!display_keys(p,keys,6) || !display_int(p,"revision",0,INT32_MAX,&revision) ||
       !display_int(p,"slot",0,M1_DISPLAY_LANES-1,&slot) || !display_int(p,"ttl_seconds",0,60,&ttl))return false;
    float value=0;int unit=0,color=-1;
    if(!ttl){
        static const char *const clear_keys[]={"revision","slot","ttl_seconds"};
        if(!display_keys(p,clear_keys,3))return false;
        m1_display_lane_config_t lane;uint32_t current;m1_display_lane_get(slot,&lane,NULL,&current);unit=lane.unit;
    }else{
        unit=display_enum(display_get(p,"unit"),unit_names,M1_DISPLAY_UNIT_COUNT);
        if(unit<0 || !display_number(display_get(p,"value"),&value))return false;
        ph_value v=display_get(p,"color");
        if(phv_exists(&v)){color=display_enum(v,color_names,4);if(color<0)return false;}
    }
    *code=m1_display_data_save(slot,revision,value,unit,ttl,color);
    return !strcmp(*code,"accepted");
}
static bool display_value_set(ph_value p,const char **code)
{
    static const char *const keys[]={"revision","slot","number","decimal","show_label","show_unit","ttl_seconds","lamps"};
    static const char *const clear_keys[]={"revision","slot","ttl_seconds"};
    *code="invalid_params";unsigned revision,slot,ttl,number=0,format=0;uint16_t lamps=0;
    if(!display_keys(p,keys,8) || !display_int(p,"revision",0,INT32_MAX,&revision) ||
       !display_int(p,"slot",0,M1_DISPLAY_LANES-1,&slot) || !display_int(p,"ttl_seconds",0,60,&ttl))return false;
    if(!ttl){
        if(!display_keys(p,clear_keys,3))return false;
    }else{
        static const char *const flags[]={"decimal","show_label","show_unit"};
        if(!display_int(p,"number",0,999,&number) || !display_lamps_parse(display_get(p,"lamps"),&lamps))return false;
        for(unsigned i=0;i<3;i++){
            ph_value v=display_get(p,flags[i]);
            if((v.kind!=PHV_TRUE&&v.kind!=PHV_FALSE))return false;
            if(v.kind==PHV_TRUE)format|=1u<<i;
        }
        if(slot==1 && (format&1u))return false;
    }
    *code=m1_display_frame_save(slot,revision,number,format,lamps,ttl);
    return !strcmp(*code,"accepted");
}

bool m1_display_mp(const char *action,const ph_value *p,ph_writer *w,const char **code)
{
    bool accepted=true;
    if(!strcmp(action,"display_profile.get")||!strcmp(action,"display_source.get")){*code=phv_keys(p,"")?"ok":"invalid_params";accepted=!strcmp(*code,"ok");}
    else if(!strcmp(action,"display_profile.set")||!strcmp(action,"display_source.set"))accepted=display_profile_set(*p,code);
    else if(!strcmp(action,"display_data.set"))accepted=display_data_set(*p,code);
    else if(!strcmp(action,"display_value.set"))accepted=display_value_set(*p,code);
    else return false;
    if(accepted)m1_display_profile_write(w);return true;
}
