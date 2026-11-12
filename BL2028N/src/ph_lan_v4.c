/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Enabled only after the product disk/auth migration and owner wiring. */
#ifdef PH_S30_LAN_V4
#include "ph_lan_v4.h"
#include "memory_policy.h"
#include "../common/lan_policy.h"
#include <string.h>

/* Shared pair handlers require synchronous device access. This pointer exists
 * only inside feed(), under the single product/network owner, never a queue. */
static ph_lan_v4 *pair_owner;
#define PH_LAN_AUTH_TYPES_PROVIDED
#define AUTH_DEVICE_ID (pair_owner->local)
#define device_local_auth() (pair_owner ? pair_owner->auth : NULL)
#define feature_lan_context s30_lan_auth_context
#define feature_lan_auth s30_lan_auth_check
#define feature_lan_pair s30_lan_auth_pair
#include "../common/lan_auth.inc"
#undef feature_lan_context
#undef feature_lan_auth
#undef feature_lan_pair
#undef device_local_auth
#undef AUTH_DEVICE_ID
#include "../common/lan_wire_v4.inc"

static uint32_t port_now(void *user)
{ph_lan_v4 *s=user;return s->ops.now(s->ops.user);}
static bool port_authorize(void *user,const char *peer,const uint8_t token[16],ph_auth_context *context)
{
    ph_lan_v4 *s=user;char text[33];lan_auth_hex(token,text);
    bool ok=ph_auth_udp_check(s->auth,peer,text,context);
    if(ok&&context->source==PH_AUTH_UDP_MQTT&&
       !(context->scopes&PH_AUTH_SCOPE_CONTROL)&&(context->scopes&PH_AUTH_SCOPE_SENSOR))
        ok=context->slot<PH_AUTH_MQTT_SLOTS&&s->input_boot&&
           s->mqtt_boot[context->slot]==s->input_boot;
    memset(text,0,sizeof(text));return ok;
}
static bool port_valid(void *user,const ph_auth_context *context)
{
    ph_lan_v4 *s=user;
    return context&&context->owner==s->auth&&
           (context->source==PH_AUTH_UDP_PAIR||context->source==PH_AUTH_UDP_MQTT)&&
           (ph_auth_context_valid(context,PH_AUTH_SCOPE_CONTROL)||
            ph_auth_context_valid(context,PH_AUTH_SCOPE_SENSOR));
}
static bool port_digest(void *user,const void *bytes,size_t length,uint8_t digest[32])
{ph_lan_v4 *s=user;return s->ops.digest(s->ops.user,bytes,length,digest);}
static bool port_send(void *user,const char *ip,uint16_t port,const void *bytes,size_t length)
{
    ph_lan_v4 *s=user;(void)ip;(void)port;
    if(bytes!=s->frame||length>s->frame_capacity||s->frame_length)return false;
    s->frame_length=length;return true;
}
static size_t error_reply(const char *id,const char *code,void *output,size_t capacity)
{
    ph_writer w;struct printbuf b;
    if(!phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,output,capacity))return 0;
    phw_begin(&w,NULL,false);phw_text(&w,"request_id",id);phw_text(&w,"code",code);
    phw_begin(&w,"data",false);phw_end(&w);phw_end(&w);return phw_finish_fixed(&w);
}
static bool receipt_key(ph_lan_v4 *s,ph_lan_v4_request *request,const ph_auth_context *context)
{
    unsigned char bytes[32+64],digest[32];static const char hex[]="0123456789abcdef";
    ph_lan_receipt_domain(bytes,context);size_t length=strlen(request->id);
    memcpy(bytes+32,request->id,length);
    if(!s->ops.digest(s->ops.user,bytes,32+length,digest))return false;
    for(unsigned i=0;i<32;i++){request->receipt_key[2*i]=hex[digest[i]>>4];request->receipt_key[2*i+1]=hex[digest[i]&15];}
    request->receipt_key[64]=0;return true;
}
static struct printbuf *port_dispatch(void *user,const void *bytes,size_t length,
                                     const ph_auth_context *context,bool *admitted)
{
    ph_lan_v4 *s=user;ph_lan_v4_request request;ph_value root,value;const char *error=NULL;
    memset(&request,0,sizeof(request));request.bytes=bytes;request.length=length;
    *admitted=false;
    if(!phv_parse(PH_WIRE_MSGPACK,bytes,length,&root)||root.kind!=PHV_MAP||
       !phv_keys(&root,"|action||params||request_id|")||
       !phv_get(&root,"request_id",&value)||!phv_text(&value,request.id,sizeof(request.id))||!request.id[0]||
       !phv_get(&root,"action",&value)||!phv_text(&value,request.action,sizeof(request.action))||
       !phv_get(&root,"params",&request.params)||request.params.kind!=PHV_MAP)error="invalid_params";
    else if(!port_valid(s,context))error="auth_required";
    else if(!ph_lan_action_allowed(request.action))error="unsupported";
    else if(!receipt_key(s,&request,context))return NULL;
    /* Include both allocator blocks and metadata before any product mutation.
     * The fixed reserve is shared with HTTP/MQTT; no lower UDP-only margin. */
    if(s->ops.free_heap(s->ops.user)<PH_SERVICE_MEMORY_RESERVE+4096u+sizeof(struct printbuf)+64u)return NULL;
    ph_writer reserved;
    if(!phw_init_capacity(&reserved,PH_WIRE_MSGPACK,4096u))return NULL;
    size_t written;
    if(error){*admitted=true;written=error_reply(request.id,error,reserved.wire->buf,4096u);}
    else written=s->ops.dispatch(s->ops.user,&request,context,reserved.wire->buf,4096u,admitted);
    if(!*admitted||!written||written>=4096u){phw_discard(&reserved);return NULL;}
    reserved.wire->bpos=(int)written;return phw_take(&reserved);
}
static lan_v4_ops server_ops(ph_lan_v4 *s)
{
    lan_v4_ops ops={s->local,s->boot,port_now,port_authorize,port_valid,port_digest,port_dispatch,port_send,s};
    return ops;
}
bool ph_lan_v4_init(ph_lan_v4 *s,const char *local,uint32_t boot,ph_auth_state *auth,const ph_lan_v4_ops *ops)
{
    if(!s||!local||!lv4_id(local)||!boot||!auth||!ops||!ops->now||!ops->free_heap||!ops->digest||!ops->dispatch)return false;
    memset(s,0,sizeof(*s));strcpy(s->local,local);s->boot=boot;s->auth=auth;s->ops=*ops;return true;
}
bool ph_lan_v4_sensor_grant(ph_lan_v4 *s,const char *peer,const char *token,uint32_t boot)
{
    if(!s||!s->auth||s->feeding||!boot||!ph_auth_mqtt_grant(s->auth,peer,token,PH_AUTH_SCOPE_SENSOR))return false;
    /* The serialized auth grant has installed exactly this peer/token in an
     * MQTT slot. Record the authoritative boot before any UDP can enter. */
    for(unsigned i=0;i<PH_AUTH_MQTT_SLOTS;i++)
        if(s->auth->mqtt[i].active&&!strcmp(s->auth->mqtt[i].peer,peer)){
            s->mqtt_boot[i]=boot;return true;
        }
    return false;
}
size_t ph_lan_v4_feed(ph_lan_v4 *s,const void *bytes,size_t length,const char *ip,uint16_t port,void *frame,size_t capacity)
{
    if(!s||s->feeding||pair_owner||!bytes||!length||length>LAN_V4_FRAME_MAX||!ip||!port||!frame||capacity<LAN_V4_FRAME_MAX)return 0;
    ph_value root;
    if(!phv_parse(PH_WIRE_MSGPACK,bytes,length,&root)||root.kind!=PHV_MAP)return 0;
    s->feeding=true;s->frame=frame;s->frame_capacity=capacity;s->frame_length=0;pair_owner=s;
    ph_value version;lan_v4_ops ops=server_ops(s);
    if(phv_get(&root,"v",&version)){
        lan_v4_packet packet;
        if(lan_v4_decode_value(&root,&packet)){
            s->input_boot=packet.boot;
            lan_v4_server_input(&s->server,&ops,&packet,ip,port,frame,capacity);
        }
    }else{
        ph_writer reply;struct printbuf b;
        /* fixed-init writes the first byte. Restore it until the shared pair
         * handler has copied its borrowed request fields, allowing aliased IO. */
        uintptr_t in=(uintptr_t)bytes,out=(uintptr_t)frame;
        bool shares_first=out>=in&&out-in<length;
        unsigned char first=shares_first?*(unsigned char *)frame:0;
        bool initialized=phw_init_fixed(&reply,PH_WIRE_MSGPACK,&b,frame,capacity);
        if(shares_first)*(unsigned char *)frame=first;
        if(initialized&&s30_lan_auth_pair(&root,&reply,ip))
            s->frame_length=phw_finish_fixed(&reply);
    }
    size_t result=s->frame_length;pair_owner=NULL;s->feeding=false;s->input_boot=0;s->frame=NULL;s->frame_capacity=0;s->frame_length=0;return result;
}
void ph_lan_v4_poll(ph_lan_v4 *s)
{if(s&&!s->feeding){lan_v4_ops ops=server_ops(s);lan_v4_server_expire(&s->server,&ops);}}
void ph_lan_v4_pause(ph_lan_v4 *s)
{if(s&&!s->feeding)lan_v4_server_release(&s->server);}
void ph_lan_v4_revoke(ph_lan_v4 *s)
{if(s&&!s->feeding){lan_v4_server_release(&s->server);memset(&s->server,0,sizeof(s->server));memset(s->mqtt_boot,0,sizeof(s->mqtt_boot));s->input_boot=0;}}
#endif
