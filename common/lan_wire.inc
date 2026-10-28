/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lan_wire.h"
#include <string.h>
static bool lan_u32(const ph_value *map,const char *key,uint32_t *out)
{
    ph_value v;int64_t n;
    if(!phv_get(map,key,&v)||!phv_int(&v,&n)||n<0||n>UINT32_MAX)return false;
    *out=(uint32_t)n;return true;
}
static bool lan_id(const ph_value *map,const char *key,char out[13])
{
    ph_value v;
    if(!phv_get(map,key,&v)||!phv_text(&v,out,13)||strlen(out)!=12)return false;
    for(unsigned i=0;i<12;i++)if(!((out[i]>='0'&&out[i]<='9')||(out[i]>='A'&&out[i]<='F')))return false;
    return true;
}
bool lan_decode(const void *data,size_t length,lan_packet *out)
{
    static const char *ops[]={"","read","probe","sample","pong"};
    ph_value root,v,body;lan_packet p={0};uint32_t version;
    if(!out||!phv_parse(PH_WIRE_MSGPACK,data,length,&root)||root.kind!=PHV_MAP||
       !phv_keys(&root,"|v||src||dst||boot||id||op||token||body|")||
       !lan_u32(&root,"v",&version)||version!=3||!lan_id(&root,"src",p.src)||
       !lan_id(&root,"dst",p.dst)||!strcmp(p.src,p.dst)||
       !lan_u32(&root,"boot",&p.boot)||!p.boot||!lan_u32(&root,"id",&p.id)||!p.id||
       !phv_get(&root,"op",&v))return false;
    for(unsigned i=1;i<=LAN_PONG;i++)if(phv_equal_text(&v,ops[i]))p.op=(lan_operation)i;
    const void *token;size_t n;
    if(!p.op||!phv_get(&root,"token",&v)||!phv_bytes(&v,&token,&n)||n!=16)return false;
    unsigned bits=0;for(unsigned i=0;i<16;i++)bits|=((const uint8_t*)token)[i];
    if(!bits)return false;
    memcpy(p.token,token,16);
    bool have_body=phv_get(&root,"body",&body);
    if(p.op!=LAN_SAMPLE){if(have_body)return false;*out=p;return true;}
    uint32_t fields;
    if(!have_body||body.kind!=PHV_MAP||!phv_keys(&body,"|boot||sequence||fields||co2_supported||values||ages|")||
       !lan_u32(&body,"boot",&p.sample.boot)||p.sample.boot!=p.boot||
       !lan_u32(&body,"sequence",&p.sample.sequence)||!lan_u32(&body,"fields",&fields)||fields>31||
       !phv_get(&body,"co2_supported",&v)||!phv_bool(&v,&p.sample.co2_supported))return false;
    if((fields&16)&&!p.sample.co2_supported)return false;
    p.sample.fields=(uint8_t)fields;
    ph_value values,ages;
    if(!phv_get(&body,"values",&values)||values.kind!=PHV_ARRAY||values.count!=5||
       !phv_get(&body,"ages",&ages)||ages.kind!=PHV_ARRAY||ages.count!=5)return false;
    for(unsigned i=0;i<5;i++){
        int64_t x,a;
        if(!phv_at(&values,i,&v)||!phv_int(&v,&x)||x<INT32_MIN||x>INT32_MAX||
           !phv_at(&ages,i,&v)||!phv_int(&v,&a)||a<0||a>UINT32_MAX)return false;
        if((fields&(1u<<i))&&((i==0&&(x < -40000||x>125000))||
           (i==1&&(x<0||x>100000))||(i>=2&&x<0)||(i==4&&(x==0||x>65535))))return false;
        p.sample.value[i]=(int32_t)x;p.sample.age_ms[i]=(uint32_t)a;
    }
    *out=p;return true;
}
bool lan_encode(ph_writer *w,const lan_packet *p)
{
    static const char *ops[]={"","read","probe","sample","pong"};
    if(!w||!p||p->op<LAN_READ||p->op>LAN_PONG)return false;
    phw_begin(w,NULL,false);phw_uint(w,"v",3);phw_text(w,"src",p->src);phw_text(w,"dst",p->dst);
    phw_u32(w,"boot",p->boot);phw_u32(w,"id",p->id);phw_text(w,"op",ops[p->op]);
    phw_bytes_value(w,"token",p->token,16);
    if(p->op==LAN_SAMPLE){
        phw_begin(w,"body",false);phw_u32(w,"boot",p->sample.boot);phw_u32(w,"sequence",p->sample.sequence);
        phw_uint(w,"fields",p->sample.fields);phw_bool(w,"co2_supported",p->sample.co2_supported);
        phw_begin(w,"values",true);for(unsigned i=0;i<5;i++)phw_i32(w,NULL,p->sample.value[i]);phw_end(w);
        phw_begin(w,"ages",true);for(unsigned i=0;i<5;i++)phw_u32(w,NULL,p->sample.age_ms[i]);phw_end(w);
        phw_end(w);
    }
    phw_end(w);return !w->failed;
}
