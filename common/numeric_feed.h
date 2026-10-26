/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_NUMERIC_FEED_H
#define PH_NUMERIC_FEED_H
#include "json_number_checked.h"
#include "sensor_field.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Declarative numeric publication. Source values are never display values.
 * The descriptors belong to the producer, not to a consumer model table. */
typedef struct {
    const char *id, *name, *unit;
    float value;
    uint32_t age_ms;
    bool valid;
} numeric_feed_field_t;

static inline bool numeric_feed_quote(char **p,size_t *left,const char *s)
{
    if(*left<3)return false;
    *(*p)++='"';--*left;
    for(;*s;s++){
        if(*left<3)return false;
        if(*s=='"' || *s=='\\'){*(*p)++='\\';--*left;}
        *(*p)++=*s;--*left;
    }
    *(*p)++='"';--*left;**p=0;return true;
}

/* Identity comes from the fixed product name and canonical account MAC, not
 * from an API parameter. A failure discards the complete envelope; callers
 * must not publish a prefix. */
static inline bool numeric_feed_encode(char *out,size_t capacity,const char *model,
        const char *id,uint32_t boot,uint32_t sequence,
        const numeric_feed_field_t *fields,unsigned count)
{
    if(!boot || !sequence || count>SENSOR_FIELD_MAX || (count && !fields))return false;
    int n=snprintf(out,capacity,"{\"schema\":2,\"model\":\"%s\",\"device_id\":\"%s\",\"boot\":%lu,\"sequence\":%lu,\"sensors\":{",
        model,id,(unsigned long)boot,(unsigned long)sequence);
    if(n<0 || (size_t)n>=capacity)return false;
    char *p=out+n;size_t left=capacity-(size_t)n;
    for(unsigned i=0;i<count;i++){
        const numeric_feed_field_t *f=fields+i;char number[32]="null";
        if(!sensor_field_token(f->id,strlen(f->id),SENSOR_FIELD_ID_MAX) ||
           !sensor_field_text(f->name,strlen(f->name),1,SENSOR_FIELD_NAME_MAX) ||
           !sensor_field_text(f->unit,strlen(f->unit),0,SENSOR_FIELD_UNIT_MAX))return false;
        bool valid=f->valid && isfinite(f->value);
        if(valid && json_number_format_float32(number,sizeof(number),f->value)<0)return false;
        n=snprintf(p,left,"%s\"%s\":{\"type\":\"number\",\"name\":",i?",":"",f->id);
        if(n<0 || (size_t)n>=left)return false;
        p+=n;left-=n;
        if(!numeric_feed_quote(&p,&left,f->name))return false;
        n=snprintf(p,left,",\"unit\":");
        if(n<0 || (size_t)n>=left)return false;
        p+=n;left-=n;
        if(!numeric_feed_quote(&p,&left,f->unit))return false;
        n=snprintf(p,left,",\"value\":%s,\"valid\":%s,\"age_ms\":%lu}",number,
            valid?"true":"false",(unsigned long)f->age_ms);
        if(n<0 || (size_t)n>=left)return false;
        p+=n;left-=n;
    }
    return left>=3 && snprintf(p,left,"}}") == 2;
}
#endif
