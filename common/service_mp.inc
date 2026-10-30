/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included once by service.c. No JSON views/objects cross this API. */
#include "service_mp.h"

typedef struct { char id[65],receipt_key[65],action[48]; ph_value params; } mp_request;
/* Receipt bytes never escape command_lock. Similar frozen replies share one
 * complete base plus literal byte differences, not mutable product state.
 * A base is always complete: references never form chains. */
typedef struct mp_receipt_frame {
    struct mp_receipt_frame *base;
    uint16_t references,length,encoded;
    unsigned char bytes[];
} mp_receipt_frame;
typedef struct { char id[65]; unsigned char digest[32]; mp_receipt_frame *response; bool lan; } mp_receipt;
static mp_receipt mp_receipts[8];
static unsigned mp_receipt_next;
static uint32_t mp_generation[2]={1,1};
static struct { uint32_t generation,freshness; ph_buf *payload; } mp_cache[2];
uint32_t service_cache_revision(void)
{
    if(!ready)return 0;
    mico_rtos_lock_mutex(&service_lock);uint32_t n=mp_generation[0];mico_rtos_unlock_mutex(&service_lock);return n;
}

void service_cache_changed(unsigned mask)
{
    if(!ready)return;
    mico_rtos_lock_mutex(&service_lock);
    for(unsigned i=0;i<2;i++)if(mask&(1u<<i))if(!++mp_generation[i])++mp_generation[i];
    mico_rtos_unlock_mutex(&service_lock);
}
static bool mp_cache_evict(void)
{
    bool freed=false;
    if(!service_command_enter(true))return false;
    for(unsigned i=0;i<2;i++)if(mp_cache[i].payload){
        ph_buf_free(mp_cache[i].payload);mp_cache[i].payload=NULL;freed=true;
    }
    service_command_leave();
    return freed;
}
static unsigned mp_receipt_u16(const unsigned char *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static void mp_receipt_put_u16(unsigned char *p,unsigned n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static void mp_receipt_apply(unsigned char *out,const mp_receipt_frame *frame)
{
    for(unsigned i=0;i<frame->encoded;){
        unsigned offset=mp_receipt_u16(frame->bytes+i),n=mp_receipt_u16(frame->bytes+i+2);i+=4;
        memcpy(out+offset,frame->bytes+i,n);i+=n;
    }
}
/* Remove the table reference before releasing. Promote the last dependent
 * in place so an orphaned base cannot retain a second full-sized allocation.
 * Transport replies are independent copies and never borrow these bytes. */
static void mp_receipt_release(mp_receipt_frame *frame)
{
    if(!frame)return;
    mp_receipt_frame *base=frame->base?frame->base:frame;
    if(frame->base)ph_wire_free(frame);
    if(!--base->references){ph_wire_free(base);return;}
    if(base->references==1)for(unsigned i=0;i<8;i++){
        mp_receipt_frame *last=mp_receipts[i].response;
        if(last&&last->base==base){
            mp_receipt_apply(base->bytes,last);mp_receipts[i].response=base;ph_wire_free(last);break;
        }
    }
}
static unsigned mp_receipt_difference(const ph_buf *wire,const mp_receipt_frame *base,unsigned char *out)
{
    unsigned encoded=0,n=(unsigned)wire->len;
    for(unsigned i=0;i<n;){
        if((unsigned char)wire->buf[i]==base->bytes[i]){i++;continue;}
        unsigned start=i++;
        while(i<n&&(unsigned char)wire->buf[i]!=base->bytes[i])i++;
        unsigned count=i-start;
        /* Poor sharing keeps the compact, single-allocation complete frame.
         * This also bounds sizing work for unrelated replies. */
        if(encoded+4+count>=n/2-sizeof(mp_receipt_frame))return n;
        if(out){mp_receipt_put_u16(out+encoded,start);mp_receipt_put_u16(out+encoded+2,count);memcpy(out+encoded+4,wire->buf+start,count);}
        encoded+=4+count;
    }
    return encoded;
}
static mp_receipt_frame *mp_receipt_save(const ph_buf *wire)
{
    if(!wire||wire->len>PHW_MAX_BYTES)return NULL;
    mp_receipt_frame *base=NULL,*seen[8];unsigned seen_count=0,encoded=(unsigned)wire->len;
    if(wire->len>=256)for(unsigned i=0;i<8;i++){
        mp_receipt_frame *candidate=mp_receipts[i].response;
        if(!candidate)continue;
        if(candidate->base)candidate=candidate->base;
        if(candidate->length!=wire->len)continue;
        bool duplicate=false;for(unsigned j=0;j<seen_count;j++)if(seen[j]==candidate)duplicate=true;
        if(duplicate)continue;seen[seen_count++]=candidate;
        unsigned cost=mp_receipt_difference(wire,candidate,NULL);
        if(cost<wire->len/2){base=candidate;encoded=cost;break;}
    }
    mp_receipt_frame *frame=ph_wire_malloc(sizeof(*frame)+encoded);
    if(!frame)return NULL;
    frame->base=base;frame->references=1;frame->length=(uint16_t)wire->len;frame->encoded=(uint16_t)encoded;
    if(base){mp_receipt_difference(wire,base,frame->bytes);base->references++;}
    else memcpy(frame->bytes,wire->buf,wire->len);
    return frame;
}
static ph_buf *mp_receipt_copy(const mp_receipt_frame *frame)
{
    ph_buf *copy=ph_buf_new((size_t)frame->length+1);
    if(!copy)return NULL;
    memcpy(copy->buf,frame->base?frame->base->bytes:frame->bytes,frame->length);
    if(frame->base)mp_receipt_apply((unsigned char *)copy->buf,frame);
    copy->len=frame->length;copy->buf[copy->len]=0;return copy;
}
static const char *mp_message(const char *code)
{
    return !strcmp(code,"ok")?"操作成功":!strcmp(code,"accepted")?"请求已接受":
        !strcmp(code,"busy")?"设备忙":!strcmp(code,"unsupported")?"不支持此功能":
        !strcmp(code,"conflict")?"请求编号冲突":!strcmp(code,"not_found")?"未找到目标":!strcmp(code,"storage_error")?"配置保存失败":"参数无效";
}
static ph_buf *mp_envelope(const char *id,const char *code,const ph_buf *data)
{
    ph_writer w;
    /* One bounded allocation for payload plus map/text headers. The payload
     * is already frozen; copying it must not require old+new growth buffers. */
    size_t overhead=64+(id?strlen(id):0)+strlen(code)+strlen(mp_message(code));
    size_t capacity=data && data->len<=PHW_MAX_BYTES-overhead ? data->len+overhead : data?PHW_MAX_BYTES+1:overhead;
    if(!phw_init_capacity(&w,PH_WIRE_MSGPACK,capacity))return NULL;
    phw_begin(&w,NULL,false);
    if(id && *id)phw_text(&w,"request_id",id);
    phw_text(&w,"code",code);phw_text(&w,"message",mp_message(code));
    if(data)phw_cached_value(&w,"data",data,8);
    else{phw_begin(&w,"data",false);phw_end(&w);}
    phw_end(&w);return phw_take(&w);
}
ph_buf *service_error_mp(const char *id,const char *code){return mp_envelope(id,code,NULL);}
static bool mp_decode(const void *data,size_t length,mp_request *request)
{
    ph_value root,v;memset(request,0,sizeof(*request));
    if(!ready || !phv_parse(PH_WIRE_MSGPACK,data,length,&root) || root.kind!=PHV_MAP ||
       !phv_keys(&root,"|request_id||action||params|"))return false;
    if(phv_get(&root,"request_id",&v) && (!phv_text(&v,request->id,sizeof(request->id)) || !identifier(request->id,sizeof(request->id))))return false;
    return phv_get(&root,"action",&v) && phv_text(&v,request->action,sizeof(request->action)) &&
        identifier(request->action,sizeof(request->action)) &&
        phv_get(&root,"params",&request->params) && request->params.kind==PHV_MAP;
}
static bool mp_uint(const ph_value *p,const char *key,int *out,int maximum)
{
    ph_value v;int64_t n;if(!phv_get(p,key,&v))return true;
    if(!phv_int(&v,&n)||n<0||n>maximum)return false;*out=(int)n;return true;
}
static void mp_logs(const ph_value *p,ph_writer *w,const char **code)
{
    int after=0,limit=8;
    if(!phv_keys(p,"|after||limit|")||!mp_uint(p,"after",&after,INT32_MAX)||!mp_uint(p,"limit",&limit,8)||!limit){*code="invalid_params";phw_begin(w,NULL,false);phw_end(w);return;}
    service_record_t selected[8];unsigned count=0;uint32_t latest,first,next=(uint32_t)after;
    mico_rtos_lock_mutex(&service_lock);latest=sequence;first=latest>=LOG_COUNT?latest-LOG_COUNT+1:1;
    for(uint32_t i=first;i<=latest&&count<(unsigned)limit;i++)if(i>(uint32_t)after){selected[count++]=records[(i-1)%LOG_COUNT];next=i;}
    mico_rtos_unlock_mutex(&service_lock);
    phw_begin(w,NULL,false);phw_bool(w,"overwritten",after&&(uint32_t)after<first-1);
    phw_u32(w,"next",next);phw_u32(w,"latest",latest);phw_begin(w,"records",true);
    for(unsigned i=0;i<count;i++){
        const service_record_t *r=&selected[i];char text[40];const char *event=log_event_text(r,text);
        phw_begin(w,NULL,false);phw_u32(w,"sequence",r->sequence);phw_u32(w,"uptime_ms",r->uptime);
        if(r->utc)phw_u32(w,"utc",r->utc);
        phw_text(w,"level",log_levels[r->level]);phw_text(w,"module",log_modules[r->module]);phw_text(w,"event",event);phw_text(w,"message",event);phw_end(w);
    }
    phw_end(w);phw_end(w);*code="ok";
}
/* Implementations copy anything retained by an asynchronous job. */
extern bool service_product_mqtt_mp(const char *,const ph_value *,ph_writer *,const char **) __attribute__((weak));
extern bool automation_mp(const char *,const ph_value *,ph_writer *,const char **);
extern bool feature_lan_pair_start(const char *,const char *,const char *,unsigned,const char **) __attribute__((weak));
extern bool feature_lan_pair_status(const char *,ph_writer *) __attribute__((weak));
static bool update_command_mp(const char *,const ph_value *,ph_writer *,const char **);
static void mp_empty(ph_writer *w){phw_begin(w,NULL,false);phw_end(w);}
static ph_buf *mp_cached(unsigned index)
{
    uint32_t generation,freshness=index?(uint32_t)device_config_recovery():mico_rtos_get_time()/1000u;
    mico_rtos_lock_mutex(&service_lock);generation=mp_generation[index];mico_rtos_unlock_mutex(&service_lock);
    /* Status contains ages/countdowns/uptime: elapsed seconds are real changes,
     * even without an external event. Capabilities remain purely event-driven. */
    if(mp_cache[index].payload && mp_cache[index].generation==generation &&
       mp_cache[index].freshness==freshness)return mp_cache[index].payload;
    ph_writer w;
    ph_buf *previous=mp_cache[index].payload;
    /* Every reference stays under command_lock; no transport borrows cache
     * storage. Invalidate before overwriting, so any failed capture discards
     * its partial bytes and a later read captures current state again. */
    mp_cache[index].payload=NULL;
    if(previous){memset(&w,0,sizeof(w));w.format=PH_WIRE_MSGPACK;w.wire=previous;previous->len=0;previous->buf[0]=0;}
    else if(!phw_init(&w,PH_WIRE_MSGPACK))return NULL;
    if(index==0)service_product_status_mp(&w);
    else service_product_capabilities_mp(&w,update_capability_names,false);
    ph_buf *next=phw_take(&w);if(!next)return NULL;
    mp_cache[index].payload=next;mp_cache[index].generation=generation;mp_cache[index].freshness=freshness;
    return next;
}
/* A build without local-auth integration must fail closed for the LAN API. */
extern bool ph_auth_context_valid(const ph_auth_context *,unsigned) __attribute__((weak));
static bool service_lan_context_valid(const ph_auth_context *context)
{
    return context&&ph_auth_context_valid&&
        (context->source==PH_AUTH_UDP_PAIR||context->source==PH_AUTH_UDP_MQTT)&&
        ph_auth_context_valid(context,PH_AUTH_SCOPE_CONTROL);
}
static ph_buf *mp_dispatch_authenticated(mp_request *r,const void *raw,size_t length,bool mqtt,const ph_auth_context *lan,bool *admitted,const ph_auth_context *http)
{
    const char *action=r->action,*code="invalid_params";const ph_value *p=&r->params;
    bool query=strlen(action)>4&&!strcmp(action+strlen(action)-4,".get");
    if((!strcmp(action,"diagnostics.get")||!strcmp(action,"capabilities.get")||!strcmp(action,"info.get")||!strcmp(action,"ota.status.get")||!strcmp(action,"ota.cancel"))&&!phv_keys(p,""))return service_error_mp(r->id,"invalid_params");
    if(!strcmp(action,"status.get")){
        ph_value view;if(!phv_keys(p,"|view|")||(phv_get(p,"view",&view)&&!phv_equal_text(&view,"full")&&!phv_equal_text(&view,"live")))return service_error_mp(r->id,"invalid_params");
    }
    if(device_config_recovery() && !query && strcmp(action,"system.reset") && strcmp(action,"system.reboot") && strcmp(action,"wifi.scan"))return service_error_mp(r->id,"migration_required");
    if(device_config_recovery()&&!strcmp(action,"system.reset")){ph_value full;bool yes;if(!phv_get(p,"full",&full)||!phv_bool(&full,&yes)||!yes)return service_error_mp(r->id,"migration_required");}
    bool receipt=r->id[0]&&!query&&strncmp(action,"config.",7)&&strncmp(action,"wifi.",5)&&strncmp(action,"ota.",4)&&strcmp(action,"sensor.session.set")&&strncmp(action,"lan.pair",8);
    unsigned char digest[32];
    if(receipt){SHA256Context sha;if(SHA256Reset(&sha)||SHA256Input(&sha,raw,length)||SHA256Result(&sha,digest))return service_error_mp(r->id,"busy");}
    if(!service_command_enter(lan==NULL)){if(admitted)*admitted=false;return service_error_mp(r->id,"busy");}
    if(http&&(!ph_auth_context_valid||
       (http->source!=PH_AUTH_HTTP&&http->source!=PH_AUTH_HTTP_OPEN&&http->source!=PH_AUTH_HTTP_BASIC)||
       !ph_auth_context_valid(http,PH_AUTH_SCOPE_ADMIN))){service_command_leave();return service_error_mp(r->id,"unauthorized");}
    if(lan&&!service_lan_context_valid(lan)){service_command_leave();return service_error_mp(r->id,"auth_required");}
    const char *receipt_key=r->receipt_key[0]?r->receipt_key:r->id;
    if(receipt)for(unsigned i=0;i<8;i++)if(mp_receipts[i].lan==(lan!=NULL)&&!strcmp(mp_receipts[i].id,receipt_key)){
        ph_buf *response=!memcmp(mp_receipts[i].digest,digest,32)?(mp_receipts[i].response?mp_receipt_copy(mp_receipts[i].response):service_error_mp(r->id,"busy")):service_error_mp(r->id,"conflict");
        service_command_leave();return response;
    }
    ph_buf *payload=NULL,*response=NULL;bool owned=false,active=ota_job_active(),record_receipt=receipt;
    ph_writer w;memset(&w,0,sizeof(w));
    bool cached_query=(!strcmp(action,"status.get")||!strcmp(action,"capabilities.get"))&&!active;
    if(!cached_query&&!phw_init(&w,PH_WIRE_MSGPACK)){service_command_leave();return NULL;}
    if(!strcmp(action,"status.get")&&!active){
        ph_value view;bool valid=phv_keys(p,"|view|");
        if(phv_get(p,"view",&view))valid=valid&&(phv_equal_text(&view,"full")||phv_equal_text(&view,"live"));
        if(valid){payload=mp_cached(0);code=payload?"ok":"busy";}
    }else if(!strcmp(action,"capabilities.get")&&phv_keys(p,"")){
        if(active){service_product_capabilities_mp(&w,update_capability_names,true);code="ok";}
        else{payload=mp_cached(1);code=payload?"ok":"busy";}
    }else if(!strcmp(action,"diagnostics.get")&&!active){service_product_diagnostics_mp(&w);code="ok";}
    else if(!strcmp(action,"info.get")&&phv_keys(p,"")){service_product_info_mp(&w);code="ok";}
#ifdef PHICOMM_DEVICE_OTA
    else if(!strcmp(action,"ota.cancel")&&phv_keys(p,"")){code=ota_job_cancel()?"accepted":"busy";mp_empty(&w);}
    else if(!strcmp(action,"ota.status.get")||(!strcmp(action,"status.get")&&active)){ota_job_status_mp(&w);code="ok";}
    else if(active&&strcmp(action,"ota.check.get")&&strcmp(action,"ota.sources.get")&&strcmp(action,"ota.notes.get")){code="busy";record_receipt=false;mp_empty(&w);}
#endif
    else if(!strcmp(action,"lan.pair")||!strcmp(action,"lan.pair.get")){
        ph_value v;char peer[13],ip[16],password[33]={0};int64_t scopes;
        bool start=!strcmp(action,"lan.pair");
        bool valid=phv_keys(p,start?"|peer||ip||password||scopes|":"|peer|")&&phv_get(p,"peer",&v)&&phv_text(&v,peer,sizeof(peer));
        if(start)valid=valid&&phv_get(p,"ip",&v)&&phv_text(&v,ip,sizeof(ip))&&phv_get(p,"scopes",&v)&&phv_int(&v,&scopes)&&scopes>=1&&scopes<=7;
        if(start&&phv_get(p,"password",&v))valid=valid&&phv_text(&v,password,sizeof(password));
        if(!valid){code="invalid_params";mp_empty(&w);}
        else if(start){if(feature_lan_pair_start)feature_lan_pair_start(peer,ip,password,(unsigned)scopes,&code);else code="unsupported";mp_empty(&w);}
        else if(feature_lan_pair_status){code=feature_lan_pair_status(peer,&w)?"ok":"not_found";}
        else{code="unsupported";mp_empty(&w);}
        memset(password,0,sizeof(password));
    }
    else if(!strcmp(action,"logs.get"))mp_logs(p,&w,&code);
    else{
        bool handled=update_command_mp(action,p,&w,&code);
        if(!handled)handled=automation_mp(action,p,&w,&code);
        if(!handled&&mqtt&&service_product_mqtt_mp)handled=service_product_mqtt_mp(action,p,&w,&code);
        if(!handled)handled=service_product_mp(action,p,&w,&code);
        if(!handled){code="unsupported";mp_empty(&w);}
    }
    if(!payload && !w.wire && !phw_init(&w,PH_WIRE_MSGPACK)){service_command_leave();return NULL;}
    if(!payload){if(!w.depth && !w.wire->len)mp_empty(&w);payload=phw_take(&w);owned=true;}else phw_discard(&w);
    response=payload?mp_envelope(r->id,code,payload):NULL;if(owned)ph_buf_free(payload);
    if(record_receipt){mp_receipt *saved=&mp_receipts[mp_receipt_next++%8];mp_receipt_frame *previous=saved->response;saved->response=NULL;mp_receipt_release(previous);memset(saved,0,sizeof(*saved));strcpy(saved->id,receipt_key);memcpy(saved->digest,digest,32);saved->response=mp_receipt_save(response);saved->lan=lan!=NULL;}
    service_command_leave();
    /* Command events refresh status for both accepted and rejected writes.
     * Capabilities change only through their authoritative invalidation or
     * recovery flag, not through a display value/failed write. OTA reads use
     * their separate restricted path above. */
    if(!query)service_event(!strcmp(code,"accepted")||!strcmp(code,"ok")?"info":"warn","command",!strcmp(code,"accepted")||!strcmp(code,"ok")?"accepted":"rejected");
    return response;
}
static ph_buf *mp_dispatch(mp_request *r,const void *raw,size_t length,bool mqtt,const ph_auth_context *lan,bool *admitted)
{return mp_dispatch_authenticated(r,raw,length,mqtt,lan,admitted,NULL);}
ph_buf *service_command_mp(const void *data,size_t length)
{mp_request r;if(!mp_decode(data,length,&r))return service_error_mp(r.id,"invalid_params");return mp_dispatch(&r,data,length,false,NULL,NULL);}
ph_buf *service_http_command_mp(const void *data,size_t length,const ph_auth_context *context)
{mp_request r;if(!mp_decode(data,length,&r))return service_error_mp(r.id,"invalid_params");if(!context)return service_error_mp(r.id,"unauthorized");return mp_dispatch_authenticated(&r,data,length,false,NULL,NULL,context);}
ph_buf *service_mqtt_command_mp(const void *data,size_t length)
{mp_request r;if(!mp_decode(data,length,&r))return service_error_mp(r.id,"invalid_params");return mp_dispatch(&r,data,length,true,NULL,NULL);}
static ph_buf *mp_query(const char *action,const ph_auth_context *http)
{
    static const unsigned char empty_map=0x80;
    mp_request r={0};
    if(!action||!identifier(action,sizeof(r.action)))return NULL;
    size_t n=strlen(action);if(n<=4||strcmp(action+n-4,".get"))return NULL;
    strcpy(r.action,action);
    if(!phv_parse(PH_WIRE_MSGPACK,&empty_map,1,&r.params))return NULL;
    /* Internal queries already have typed parameters. Do not serialize and
     * parse a synthetic request just to enter the same dispatcher. */
    return mp_dispatch_authenticated(&r,NULL,0,false,NULL,NULL,http);
}
ph_buf *service_query_mp(const char *action){return mp_query(action,NULL);}
ph_buf *service_http_query_mp(const char *action,const ph_auth_context *context)
{return context?mp_query(action,context):service_error_mp(NULL,"unauthorized");}
bool service_readonly_mp(const void *data,size_t length)
{
    mp_request r;if(!mp_decode(data,length,&r))return false;
    /* Only reviewed, side-effect-free reads may charge their actual reply
     * after execution. A future action ending in .get may start network work
     * (for example an OTA check); it must keep write-style pre-admission. */
    return !strcmp(r.action,"status.get")||!strcmp(r.action,"capabilities.get")||
        !strcmp(r.action,"diagnostics.get")||!strcmp(r.action,"info.get")||
        !strcmp(r.action,"config.get")||!strcmp(r.action,"logs.get");
}
#include "service_lan.inc"
