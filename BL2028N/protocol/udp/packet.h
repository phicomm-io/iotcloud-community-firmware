#pragma once
#include "../typed_wire.h"
#include <limits.h>
#include <string.h>
static inline ph_value fv_get(ph_value o,const char *key){ph_value v={0};v.kind=PHV_MISSING;phv_get(&o,key,&v);return v;}
static inline bool fv_has(ph_value v){return v.kind!=PHV_MISSING;}
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Typed LAN/MQTT linkage values. Views borrow one admitted receive buffer;
 * retry slots own final MessagePack bytes, never views into another thread. */
typedef struct {
 uint32_t version,boot,id;
 char src[13],dst[13],op[24],token[33],group[33],domain[16],kind[16];
 ph_value body;
} feature_packet_t;
static bool fv_copy(ph_value o,const char *key,char *out,size_t size,bool required){
 ph_value v=fv_get(o,key);if(!fv_has(v)){if(size)out[0]=0;return !required;}return phv_text(&v,out,size);
}
static bool fv_u32(ph_value o,const char *key,uint32_t *out){
 ph_value v=fv_get(o,key);int64_t n;
 if(!phv_int(&v,&n)||n<INT32_MIN||n>UINT32_MAX)return false;*out=(uint32_t)n;return true;
}
static inline bool fv_flag(ph_value o,const char *key){
 ph_value v=fv_get(o,key);if(v.kind==PHV_TRUE)return true;int64_t n=0;return phv_int(&v,&n)&&n!=0;
}
static bool packet_parse(ph_value o,feature_packet_t *p){
 memset(p,0,sizeof(*p));p->body.kind=PHV_MISSING;
 return o.kind==PHV_MAP&&fv_u32(o,"v",&p->version)&&fv_u32(o,"boot",&p->boot)&&fv_u32(o,"id",&p->id)&&
 fv_copy(o,"src",p->src,sizeof(p->src),true)&&fv_copy(o,"dst",p->dst,sizeof(p->dst),true)&&
 fv_copy(o,"op",p->op,sizeof(p->op),true)&&fv_copy(o,"token",p->token,sizeof(p->token),true)&&
 fv_copy(o,"group",p->group,sizeof(p->group),false)&&fv_copy(o,"domain",p->domain,sizeof(p->domain),false)&&
 fv_copy(o,"kind",p->kind,sizeof(p->kind),false)&&(p->body=fv_get(o,"body"),true);
}
