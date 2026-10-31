/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_LAN_WIRE_V4_IMPLEMENTATION
#define PH_LAN_WIRE_V4_IMPLEMENTATION
#include "lan_wire_v4.h"
#include <string.h>
static const char *const lv4_names[]={"","command","result","pull","ack"};
static bool lv4_id(const char *s)
{
    if(!s||strlen(s)!=12)return false;
    for(unsigned i=0;i<12;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;
    return true;
}
static bool lv4_u32(const ph_value *map,const char *key,uint32_t *out)
{
    ph_value v;int64_t n;
    if(!phv_get(map,key,&v)||!phv_int(&v,&n)||n<0||n>UINT32_MAX)return false;
    *out=(uint32_t)n;return true;
}
static bool lv4_code(const char *s)
{
    static const char *const codes[]={"ok","busy","expired","conflict","too_large","auth_required","invalid_params","acked"};
    for(unsigned i=0;i<sizeof(codes)/sizeof(*codes);i++)if(!strcmp(s,codes[i]))return true;
    return false;
}
static bool lv4_valid(const lan_v4_packet *p)
{
    if(!p||!lv4_id(p->src)||!lv4_id(p->dst)||!strcmp(p->src,p->dst)||!p->boot||!p->id)return false;
    unsigned bits=0;for(unsigned i=0;i<16;i++)bits|=p->token[i];if(!bits)return false;
    if(p->op==LAN_V4_COMMAND)return p->body&&p->body_length&&p->body_length<=LAN_V4_REQUEST_MAX;
    if(p->op==LAN_V4_PULL)return p->part<(LAN_V4_RESPONSE_MAX+LAN_V4_CHUNK-1)/LAN_V4_CHUNK&&!p->body_length;
    if(p->op==LAN_V4_ACK)return !p->body_length;
    if(p->op!=LAN_V4_RESULT||!p->request_boot||!lv4_code(p->code))return false;
    if(strcmp(p->code,"ok"))return !p->body_length&&!p->size&&!p->part&&!p->total;
    return p->body&&p->size&&p->size<=LAN_V4_RESPONSE_MAX&&
        p->total==(p->size+LAN_V4_CHUNK-1)/LAN_V4_CHUNK&&p->part<p->total&&
        p->body_length==(p->part+1u==p->total?p->size-p->part*LAN_V4_CHUNK:LAN_V4_CHUNK);
}
bool lan_v4_decode_value(const ph_value *root,lan_v4_packet *out)
{
    lan_v4_packet p;ph_value v;uint32_t n;memset(&p,0,sizeof(p));
    if(!out||!root||root->kind!=PHV_MAP||!lv4_u32(root,"v",&n)||n!=4||
       !phv_get(root,"src",&v)||!phv_text(&v,p.src,sizeof(p.src))||
       !phv_get(root,"dst",&v)||!phv_text(&v,p.dst,sizeof(p.dst))||
       !lv4_u32(root,"boot",&p.boot)||!lv4_u32(root,"id",&p.id)||
       !phv_get(root,"token",&v))return false;
    const void *token;size_t length;
    if(!phv_bytes(&v,&token,&length)||length!=16)return false;memcpy(p.token,token,16);
    if(!phv_get(root,"op",&v))return false;
    for(unsigned i=LAN_V4_COMMAND;i<=LAN_V4_ACK;i++)if(phv_equal_text(&v,lv4_names[i]))p.op=(lan_v4_operation)i;
    /* Every accepted field below is mandatory. Parsing already guarantees
     * unique keys: the exact field count therefore rejects unknown fields
     * without a second map walk and repeated per-operation allowlists. */
    unsigned fields=7;
    if(p.op==LAN_V4_COMMAND){
        fields=8;
        if(!phv_get(root,"body",&v)||!phv_bytes(&v,&p.body,&p.body_length))return false;
    }else if(p.op==LAN_V4_PULL){
        fields=8;
        if(!lv4_u32(root,"part",&n)||n>UINT16_MAX)return false;p.part=(uint16_t)n;
    }else if(p.op==LAN_V4_RESULT){
        if(!lv4_u32(root,"request_boot",&p.request_boot)||!phv_get(root,"code",&v)||!phv_text(&v,p.code,sizeof(p.code)))return false;
        fields=9;
        if(!strcmp(p.code,"ok")){
            fields=13;
            if(!phv_get(root,"body",&v)||!phv_bytes(&v,&p.body,&p.body_length)||!lv4_u32(root,"part",&n)||n>UINT16_MAX)return false;p.part=(uint16_t)n;
            if(!lv4_u32(root,"total",&n)||n>UINT16_MAX)return false;p.total=(uint16_t)n;
            if(!lv4_u32(root,"size",&n)||n>UINT16_MAX)return false;p.size=(uint16_t)n;
        }
    }
    if(root->count!=fields||!lv4_valid(&p))return false;*out=p;return true;
}
bool lan_v4_decode(const void *data,size_t length,lan_v4_packet *out)
{
    ph_value root;return length<=LAN_V4_FRAME_MAX&&phv_parse(PH_WIRE_MSGPACK,data,length,&root)&&lan_v4_decode_value(&root,out);
}
bool lan_v4_encode(ph_writer *w,const lan_v4_packet *p)
{
    if(!w||w->format!=PH_WIRE_MSGPACK||!lv4_valid(p)){if(w)w->failed=true;return false;}
    phw_begin(w,NULL,false);phw_uint(w,"v",4);phw_text(w,"src",p->src);phw_text(w,"dst",p->dst);
    phw_u32(w,"boot",p->boot);phw_u32(w,"id",p->id);phw_text(w,"op",lv4_names[p->op]);phw_bytes_value(w,"token",p->token,16);
    if(p->op==LAN_V4_COMMAND)phw_bytes_value(w,"body",p->body,p->body_length);
    if(p->op==LAN_V4_PULL)phw_uint(w,"part",p->part);
    if(p->op==LAN_V4_RESULT){
        phw_u32(w,"request_boot",p->request_boot);phw_text(w,"code",p->code);
        if(!strcmp(p->code,"ok")){phw_uint(w,"part",p->part);phw_uint(w,"total",p->total);phw_uint(w,"size",p->size);phw_bytes_value(w,"body",p->body,p->body_length);}
    }
    phw_end(w);return !w->failed;
}
void lan_v4_server_release(lan_v4_server *s)
{if(s){PH_LAN_V4_BUFFER_FREE(s->response);s->response=NULL;s->response_id=0;}}
void lan_v4_server_expire(lan_v4_server *s,const lan_v4_ops *o)
{
    if(s->response&&(uint32_t)(o->now(o->user)-s->response_at)>=LAN_V4_RESPONSE_MS)lan_v4_server_release(s);
    for(unsigned i=0;i<LAN_V4_SESSIONS;i++)if(s->sessions[i].used&&!o->valid(o->user,&s->sessions[i].context)){
        if(s->response&&s->response_slot==i)lan_v4_server_release(s);
        memset(&s->sessions[i],0,sizeof(s->sessions[i]));
    }
}
static bool lv4_emit(lan_v4_server *s,const lan_v4_ops *o,const lan_v4_packet *p,
                     const char *ip,uint16_t port,const char *code,unsigned part,void *scratch,size_t cap)
{
    lan_v4_packet reply;memset(&reply,0,sizeof(reply));reply.op=LAN_V4_RESULT;
    strcpy(reply.src,o->local);strcpy(reply.dst,p->src);reply.boot=o->boot;reply.request_boot=p->boot;reply.id=p->id;
    memcpy(reply.token,p->token,16);strcpy(reply.code,code);
    if(!strcmp(code,"ok")){
        reply.size=(uint16_t)PH_LAN_V4_BUFFER_LENGTH(s->response);reply.total=(reply.size+LAN_V4_CHUNK-1)/LAN_V4_CHUNK;reply.part=(uint16_t)part;
        size_t offset=part*LAN_V4_CHUNK;if(offset>=PH_LAN_V4_BUFFER_LENGTH(s->response))return false;
        reply.body=s->response->buf+offset;reply.body_length=PH_LAN_V4_BUFFER_LENGTH(s->response)-offset;if(reply.body_length>LAN_V4_CHUNK)reply.body_length=LAN_V4_CHUNK;
    }
    ph_writer w;PH_LAN_V4_BUFFER b;
    if(cap>LAN_V4_FRAME_MAX)cap=LAN_V4_FRAME_MAX;
    if(!phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,scratch,cap)||!lan_v4_encode(&w,&reply))return false;
    size_t length=phw_finish_fixed(&w);return length&&o->send(o->user,ip,port,scratch,length);
}
bool lan_v4_server_input(lan_v4_server *s,const lan_v4_ops *o,const lan_v4_packet *p,
                         const char *ip,uint16_t port,void *scratch,size_t capacity)
{
    if(!s||!o||!p||!lv4_valid(p)||p->op==LAN_V4_RESULT||!ip||!port||!scratch||strcmp(p->dst,o->local))return false;
    lan_v4_server_expire(s,o);
    ph_auth_context context;memset(&context,0,sizeof(context));
    if(!o->authorize(o->user,p->src,p->token,&context)||!o->valid(o->user,&context))return lv4_emit(s,o,p,ip,port,"auth_required",0,scratch,capacity);
    unsigned slot=LAN_V4_SESSIONS,empty=LAN_V4_SESSIONS;
    for(unsigned i=0;i<LAN_V4_SESSIONS;i++){
        lan_v4_session *r=&s->sessions[i];if(!r->used){if(empty==LAN_V4_SESSIONS)empty=i;continue;}
        if(r->context.owner==context.owner&&r->context.source==context.source&&r->context.slot==context.slot&&r->context.generation==context.generation&&!strcmp(r->context.peer,context.peer)&&!memcmp(r->token,p->token,16)){slot=i;break;}
    }
    if(slot==LAN_V4_SESSIONS){
        if(p->op!=LAN_V4_COMMAND)return lv4_emit(s,o,p,ip,port,"expired",0,scratch,capacity);
        if(empty==LAN_V4_SESSIONS)return lv4_emit(s,o,p,ip,port,"busy",0,scratch,capacity);
        slot=empty;
    }
    lan_v4_session *r=&s->sessions[slot];
    if(r->used&&r->boot!=p->boot)return lv4_emit(s,o,p,ip,port,"conflict",0,scratch,capacity);
    if(p->op!=LAN_V4_COMMAND){
        if(!r->used||r->last_id!=p->id)return lv4_emit(s,o,p,ip,port,"expired",0,scratch,capacity);
        if(p->op==LAN_V4_ACK){if(s->response&&s->response_slot==slot&&s->response_id==p->id)lan_v4_server_release(s);return lv4_emit(s,o,p,ip,port,"acked",0,scratch,capacity);}
        if(!s->response||s->response_slot!=slot||s->response_id!=p->id)return lv4_emit(s,o,p,ip,port,"expired",0,scratch,capacity);
        if(p->part>=(PH_LAN_V4_BUFFER_LENGTH(s->response)+LAN_V4_CHUNK-1)/LAN_V4_CHUNK)return lv4_emit(s,o,p,ip,port,"invalid_params",0,scratch,capacity);
        return lv4_emit(s,o,p,ip,port,"ok",p->part,scratch,capacity);
    }
    uint8_t digest[32];if(!o->digest(o->user,p->body,p->body_length,digest))return lv4_emit(s,o,p,ip,port,"busy",0,scratch,capacity);
    if(r->used&&p->id<=r->last_id){
        if(p->id==r->last_id&&memcmp(r->digest,digest,32))return lv4_emit(s,o,p,ip,port,"conflict",0,scratch,capacity);
        const char *code=p->id==r->last_id&&s->response&&s->response_slot==slot&&s->response_id==p->id?"ok":"expired";
        return lv4_emit(s,o,p,ip,port,code,0,scratch,capacity);
    }
    if(s->response&&s->response_slot!=slot)return lv4_emit(s,o,p,ip,port,"busy",0,scratch,capacity);
    /* A new id from this owner implicitly releases its previous response.
     * Never construct a second full response while retaining the first. */
    lan_v4_server_release(s);
    bool admitted=true;PH_LAN_V4_BUFFER *response=o->dispatch(o->user,p->body,p->body_length,&context,&admitted);
    if(!admitted){PH_LAN_V4_BUFFER_FREE(response);return lv4_emit(s,o,p,ip,port,"busy",0,scratch,capacity);}
    /* Commit the high-water mark even if encoding/IO fails after a mutation. */
    r->used=true;r->context=context;r->boot=p->boot;r->last_id=p->id;
    memcpy(r->token,p->token,16);memcpy(r->digest,digest,32);
    if(!response)return lv4_emit(s,o,p,ip,port,"expired",0,scratch,capacity);
    if(!PH_LAN_V4_BUFFER_LENGTH(response)||PH_LAN_V4_BUFFER_LENGTH(response)>LAN_V4_RESPONSE_MAX){PH_LAN_V4_BUFFER_FREE(response);return lv4_emit(s,o,p,ip,port,"too_large",0,scratch,capacity);}
    s->response=response;s->response_at=o->now(o->user);s->response_id=p->id;s->response_slot=slot;
    return lv4_emit(s,o,p,ip,port,"ok",0,scratch,capacity);
}
#endif
