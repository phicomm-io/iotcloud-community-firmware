/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "typed_wire.h"
#include "sensor_field.h"
#include <math.h>
#include <string.h>
/* Envelope views are borrowed only during synchronous dispatch. Persistent
 * consumers cache selected owned descriptors, never pointers into this view. */
typedef struct { uint32_t boot,sequence,descriptor_revision; ph_value fields; } ph_sensor_view;
static inline bool ph_sensor_u32(const ph_value *o,const char *key,uint32_t *out,bool positive)
{ph_value v;int64_t n;if(!phv_get(o,key,&v)||!phv_int(&v,&n)||n<(positive?1:0)||(uint64_t)n>UINT32_MAX)return false;*out=(uint32_t)n;return true;}
static inline bool ph_sensor_view_parse(const ph_value *o,const char *id,bool metadata,ph_sensor_view *out)
{
    ph_value v;uint32_t schema;
    if(!ph_sensor_u32(o,"schema",&schema,true)||schema!=3||!phv_get(o,"device_id",&v)||!phv_equal_text(&v,id)||
       !ph_sensor_u32(o,"boot",&out->boot,true)||!ph_sensor_u32(o,"descriptor_revision",&out->descriptor_revision,true)||
       (!metadata&&!ph_sensor_u32(o,"sequence",&out->sequence,true))||!phv_get(o,metadata?"fields":"sensors",&out->fields)||out->fields.kind!=PHV_MAP||out->fields.count>SENSOR_FIELD_MAX)return false;
    ph_iterator it;char key[128];phv_iter(&out->fields,&it);
    while(phv_next(&it,key,&v))if(!sensor_field_token(key,strlen(key),SENSOR_FIELD_ID_MAX)||v.kind!=PHV_MAP)return false;
    return true;
}
static inline bool ph_sensor_descriptor_matches(const ph_sensor_view *view,const char *id,const char *unit)
{
    ph_value field,v;
    if(!phv_get(&view->fields,id,&field)||!phv_keys(&field,"|name||type||unit|")||!phv_get(&field,"type",&v)||!phv_equal_text(&v,"number")||
       !phv_get(&field,"name",&v)||v.kind!=PHV_TEXT||!sensor_field_text((const char *)v.text,v.size,1,SENSOR_FIELD_NAME_MAX)||
       !phv_get(&field,"unit",&v)||!phv_equal_text(&v,unit))return false;
    return true;
}
static inline bool ph_sensor_reading(const ph_sensor_view *view,const char *id,uint32_t lease,float *value,uint32_t *age)
{
    ph_value field,v;bool valid;double n;
    if(!phv_get(&view->fields,id,&field)||!phv_keys(&field,"|value||valid||age_ms|")||!phv_get(&field,"valid",&v)||!phv_bool(&v,&valid)||!valid||
       !ph_sensor_u32(&field,"age_ms",age,false)||*age>=lease||!phv_get(&field,"value",&v)||!phv_number(&v,&n))return false;
    *value=(float)n;return isfinite(n)&&isfinite(*value);
}
static inline void ph_sensor_metadata_begin(ph_writer *w,const char *id,uint32_t boot,uint32_t revision)
{phw_begin(w,NULL,false);phw_u32(w,"schema",3);phw_text(w,"device_id",id);phw_u32(w,"boot",boot);phw_u32(w,"descriptor_revision",revision);phw_begin(w,"fields",false);}
static inline void ph_sensor_descriptor_write(ph_writer *w,const char *id,const char *name,const char *unit)
{phw_begin(w,id,false);phw_text(w,"name",name);phw_text(w,"type","number");phw_text(w,"unit",unit);phw_end(w);}
static inline void ph_sensor_snapshot_begin(ph_writer *w,const char *id,uint32_t boot,uint32_t sequence,uint32_t revision)
{phw_begin(w,NULL,false);phw_u32(w,"schema",3);phw_text(w,"device_id",id);phw_u32(w,"boot",boot);phw_u32(w,"sequence",sequence);phw_u32(w,"descriptor_revision",revision);phw_begin(w,"sensors",false);}
static inline void ph_sensor_reading_write(ph_writer *w,const char *id,double value,bool valid,uint32_t age)
{valid=valid&&isfinite(value)&&age<20000;phw_begin(w,id,false);phw_bool(w,"valid",valid);if(valid)phw_number(w,"value",value);else phw_null(w,"value");phw_u32(w,"age_ms",age);phw_end(w);}
static inline void ph_sensor_finish(ph_writer *w){phw_end(w);phw_end(w);}
