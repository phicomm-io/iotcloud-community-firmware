#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define M1_DETAILS_STALE_MS 6000u
#define M1_DETAILS_COUNT 7
/* Metadata/data/metadata transaction. Only a coherent complete frame commits. */
typedef struct {
    uint8_t raw[32], length, flags, health;
    uint16_t generation;
    uint32_t age_ms, at;
    bool received;
} m1_sensor_frame_t;
typedef struct {
    m1_sensor_frame_t frames[M1_DETAILS_COUNT], pending;
    uint8_t id, offset, stage, diagnostic;
    bool supported;
} m1_details_t;
static inline uint16_t m1_details_u16(const uint8_t *p){return (uint16_t)p[0]*256u+p[1];}
static inline uint32_t m1_details_age(const m1_sensor_frame_t *s,uint32_t now){
    uint32_t elapsed=now-s->at;
    return s->age_ms>UINT32_MAX-elapsed?UINT32_MAX:s->age_ms+elapsed;
}
static inline bool m1_details_valid(const m1_details_t *d,uint32_t now){
    const m1_sensor_frame_t *s=&d->frames[0];
    if(!d->supported || !s->received || s->length!=32 || s->health!=1 ||
       (s->flags&31)!=3 || m1_details_age(s,now)>=M1_DETAILS_STALE_MS)return false;
    const uint8_t *p=s->raw;unsigned sum=0;
    for(unsigned i=0;i<30;i++)sum+=p[i];
    return p[0]==0x42 && p[1]==0x4d && m1_details_u16(p+2)==28 &&
        !p[29] && (uint16_t)sum==m1_details_u16(p+30);
}
static inline void m1_details_request(const m1_details_t *d,uint8_t *a){
    memset(a,0,7);a[0]=d->id;
    if(d->stage==1){a[1]=1;a[2]=d->offset;
        unsigned left=d->pending.length-d->offset;a[3]=left>9?9:left;
        a[4]=d->pending.generation>>8;a[5]=d->pending.generation;}
}
static inline void m1_details_next(m1_details_t *d){
    if(!d->id){d->diagnostic=d->diagnostic%6+1;d->id=d->diagnostic;}else d->id=0;
    d->stage=d->offset=0;
}
static inline void m1_details_fail(m1_details_t *d){
    /* A failed transfer must not leave a previous measurement usable. */
    d->frames[d->id].health=0;m1_details_next(d);
}
static inline bool m1_details_reply(m1_details_t *d,const uint8_t *p,uint32_t now){
    static const uint8_t lengths[]={32,9,6,3,6,9,8};
    if(p[4] || p[5]!=d->id){m1_details_fail(d);return true;}
    if(d->stage==1){
        unsigned n=d->pending.length-d->offset;if(n>9)n=9;
        if(p[6]!=d->offset || p[7]!=n || m1_details_u16(p+8)!=d->pending.generation){m1_details_fail(d);return true;}
        memcpy(d->pending.raw+d->offset,p+10,n);d->offset+=n;
        if(d->offset==d->pending.length)d->stage=2;
        return false;
    }
    uint32_t age=(uint32_t)p[9]<<24|(uint32_t)p[10]<<16|(uint32_t)p[11]<<8|p[12];
    if(d->stage==2){
        if(p[6]!=d->pending.length || m1_details_u16(p+7)!=d->pending.generation){m1_details_fail(d);return true;}
        d->pending.age_ms=age;d->pending.at=now;d->pending.flags=p[13];d->pending.health=p[14];
        d->pending.received=true;d->frames[d->id]=d->pending;m1_details_next(d);return true;
    }
    if(!p[6]){d->frames[d->id]=(m1_sensor_frame_t){0};m1_details_next(d);return true;}
    if(p[6]!=lengths[d->id]){m1_details_fail(d);return true;}
    d->pending=(m1_sensor_frame_t){.length=p[6],.generation=m1_details_u16(p+7)};
    d->stage=1;d->offset=0;return false;
}
