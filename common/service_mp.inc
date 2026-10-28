/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included once by service.c. No JSON views/objects cross this API. */
#include "service_mp.h"

typedef struct { char id[65],action[48]; ph_value params; } mp_request;
typedef struct { char id[65]; unsigned char digest[32]; ph_buf *response; } mp_receipt;
static mp_receipt mp_receipts[8];
static unsigned mp_receipt_next;
static uint32_t mp_generation[2]={1,1};
static struct { uint32_t generation; ph_buf *payload; } mp_cache[2];

void service_cache_changed(unsigned mask)
{
    if(!ready)return;
    mico_rtos_lock_mutex(&service_lock);
    for(unsigned i=0;i<2;i++)if(mask&(1u<<i))if(!++mp_generation[i])++mp_generation[i];
    mico_rtos_unlock_mutex(&service_lock);
}
static ph_buf *mp_copy(const ph_buf *src)
{
    if(!src)return NULL;
    ph_buf *copy=ph_buf_new(src->len+1);
    if(!copy)return NULL;
    memcpy(copy->buf,src->buf,src->len);copy->len=src->len;copy->buf[copy->len]=0;
    return copy;
}
static const char *mp_message(const char *code)
{
    return !strcmp(code,"ok")?"操作成功":!strcmp(code,"accepted")?"请求已接受":
        !strcmp(code,"busy")?"设备忙":!strcmp(code,"unsupported")?"不支持此功能":
        !strcmp(code,"conflict")?"请求编号冲突":!strcmp(code,"storage_error")?"配置保存失败":"参数无效";
}
static ph_buf *mp_envelope(const char *id,const char *code,const ph_buf *data)
{
    ph_writer w;
    if(!phw_init(&w,PH_WIRE_MSGPACK))return NULL;
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
static bool update_command_mp(const char *,const ph_value *,ph_writer *,const char **);
static void mp_empty(ph_writer *w){phw_begin(w,NULL,false);phw_end(w);}
static ph_buf *mp_cached(unsigned index)
{
    uint32_t generation;
    mico_rtos_lock_mutex(&service_lock);generation=mp_generation[index];mico_rtos_unlock_mutex(&service_lock);
    if(mp_cache[index].payload && mp_cache[index].generation==generation)return mp_cache[index].payload;
    ph_writer w;if(!phw_init(&w,PH_WIRE_MSGPACK))return NULL;
    if(index==0)service_product_status_mp(&w);
    else service_product_capabilities_mp(&w,update_capability_names,false);
    ph_buf *next=phw_take(&w);if(!next)return NULL;
    ph_buf_free(mp_cache[index].payload);mp_cache[index].payload=next;mp_cache[index].generation=generation;
    return next;
}
static ph_buf *mp_dispatch(mp_request *r,const void *raw,size_t length,bool mqtt)
{
    const char *action=r->action,*code="invalid_params";const ph_value *p=&r->params;
    bool query=strlen(action)>4&&!strcmp(action+strlen(action)-4,".get");
    if(device_config_recovery() && !query && strcmp(action,"system.reset") && strcmp(action,"system.reboot") && strcmp(action,"wifi.scan"))return service_error_mp(r->id,"migration_required");
    if(device_config_recovery()&&!strcmp(action,"system.reset")){ph_value full;bool yes;if(!phv_get(p,"full",&full)||!phv_bool(&full,&yes)||!yes)return service_error_mp(r->id,"migration_required");}
    bool receipt=r->id[0]&&!query&&strncmp(action,"config.",7)&&strncmp(action,"wifi.",5)&&strncmp(action,"ota.",4)&&strcmp(action,"sensor.session.set");
    unsigned char digest[32];
    if(receipt){SHA256Context sha;if(SHA256Reset(&sha)||SHA256Input(&sha,raw,length)||SHA256Result(&sha,digest))return service_error_mp(r->id,"busy");}
    mico_rtos_lock_mutex(&command_lock);
    if(receipt)for(unsigned i=0;i<8;i++)if(!strcmp(mp_receipts[i].id,r->id)){
        ph_buf *response=!memcmp(mp_receipts[i].digest,digest,32)?(mp_receipts[i].response?mp_copy(mp_receipts[i].response):service_error_mp(r->id,"busy")):service_error_mp(r->id,"conflict");
        mico_rtos_unlock_mutex(&command_lock);return response;
    }
    ph_buf *payload=NULL,*response=NULL;bool owned=false,active=ota_job_active();
    ph_writer w;if(!phw_init(&w,PH_WIRE_MSGPACK)){mico_rtos_unlock_mutex(&command_lock);return NULL;}
    if(!strcmp(action,"status.get")&&!active){
        ph_value view;bool valid=phv_keys(p,"|view|");
        if(phv_get(p,"view",&view))valid=valid&&(phv_equal_text(&view,"full")||phv_equal_text(&view,"live"));
        if(valid){payload=mp_cached(0);code=payload?"ok":"busy";}
    }else if(!strcmp(action,"capabilities.get")&&phv_keys(p,"")){
        if(active){service_product_capabilities_mp(&w,update_capability_names,true);code="ok";}
        else{payload=mp_cached(1);code=payload?"ok":"busy";}
    }else if(!strcmp(action,"info.get")&&phv_keys(p,"")){service_product_info_mp(&w);code="ok";}
#ifdef PHICOMM_DEVICE_OTA
    else if(!strcmp(action,"ota.cancel")&&phv_keys(p,"")){code=ota_job_cancel()?"accepted":"busy";mp_empty(&w);}
    else if((!strcmp(action,"ota.status.get")||!strcmp(action,"status.get"))&&active){ota_job_status_mp(&w);code="ok";}
    else if(active&&strcmp(action,"ota.check.get")&&strcmp(action,"ota.sources.get")&&strcmp(action,"ota.notes.get")){code="busy";mp_empty(&w);}
#endif
    else if(!strcmp(action,"logs.get"))mp_logs(p,&w,&code);
    else{
        bool handled=update_command_mp(action,p,&w,&code);
        if(!handled)handled=automation_mp(action,p,&w,&code);
        if(!handled&&mqtt&&service_product_mqtt_mp)handled=service_product_mqtt_mp(action,p,&w,&code);
        if(!handled)handled=service_product_mp(action,p,&w,&code);
        if(!handled){code="unsupported";mp_empty(&w);}
    }
    if(!payload){if(!w.depth && !w.wire->len)mp_empty(&w);payload=phw_take(&w);owned=true;}else phw_discard(&w);
    response=payload?mp_envelope(r->id,code,payload):NULL;if(owned)ph_buf_free(payload);
    if(receipt){mp_receipt *saved=&mp_receipts[mp_receipt_next++%8];ph_buf_free(saved->response);memset(saved,0,sizeof(*saved));strcpy(saved->id,r->id);memcpy(saved->digest,digest,32);saved->response=mp_copy(response);}
    mico_rtos_unlock_mutex(&command_lock);
    if(!query){service_cache_changed(SERVICE_CACHE_ALL);service_event(!strcmp(code,"accepted")||!strcmp(code,"ok")?"info":"warn","command",!strcmp(code,"accepted")||!strcmp(code,"ok")?"accepted":"rejected");}
    return response;
}
ph_buf *service_command_mp(const void *data,size_t length)
{mp_request r;if(!mp_decode(data,length,&r))return service_error_mp(r.id,"invalid_params");return mp_dispatch(&r,data,length,false);}
ph_buf *service_mqtt_command_mp(const void *data,size_t length)
{mp_request r;if(!mp_decode(data,length,&r))return service_error_mp(r.id,"invalid_params");return mp_dispatch(&r,data,length,true);}
ph_buf *service_query_mp(const char *action)
{
    ph_writer w;if(!action||!phw_init(&w,PH_WIRE_MSGPACK))return NULL;
    phw_begin(&w,NULL,false);phw_text(&w,"action",action);phw_begin(&w,"params",false);phw_end(&w);phw_end(&w);
    ph_buf *request=phw_take(&w);if(!request)return NULL;
    ph_buf *response=service_command_mp(request->buf,request->len);ph_buf_free(request);return response;
}
