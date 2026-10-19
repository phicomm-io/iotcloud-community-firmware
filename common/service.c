#include "service.h"
#include "firmware_variant.h"
#include "automation.h"
#include "ota/mode.h"
#include "mico.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "generated/build_ota.h"
#define LOG_COUNT 64
/* Fixed vocabulary events never accept credentials, payloads or URLs. */
typedef struct { uint32_t sequence, uptime, utc; int32_t argument; uint8_t level,module,event,reserved; } service_record_t;
#include "log_dictionary.h"
static service_record_t records[LOG_COUNT];
static uint32_t sequence;
static mico_mutex_t service_lock, command_lock;
static bool ready;
typedef struct { char id[65]; char *request, *response; } receipt_t;
static receipt_t receipts[8];
static unsigned receipt_next;
static char *receipt_json_copy(json_object *o) {
    const char *wire = o ? json_object_to_json_string(o) : NULL;
    return wire ? strdup(wire) : NULL;
}
static void string(json_object *o,const char *k,const char *v) { json_build_object_add(o,k,json_object_new_string(v)); }
void service_init(void) {
    if (ready) return;
    ready = mico_rtos_init_mutex(&service_lock)==kNoErr && mico_rtos_init_mutex(&command_lock)==kNoErr;
    ota_job_init();
    service_event("info","system","boot");
}
static bool identifier(const char *s,size_t max) {
    size_t n=strlen(s); if (!n || n>=max) return false;
    for (size_t i=0;i<n;i++) if (!isalnum((unsigned char)s[i]) && s[i]!='_' && s[i]!='.' && s[i]!='-') return false;
    return true;
}
void service_event(const char *level,const char *module,const char *event) {
    if (!ready || !identifier(level,8) || !identifier(module,16) || !identifier(event,40)) return;
    service_record_t encoded={0};if(!log_encode(level,module,event,&encoded))return;
    mico_rtos_lock_mutex(&service_lock);
    service_record_t *r=&records[sequence%LOG_COUNT]; *r=encoded;
    r->sequence=++sequence; r->uptime=mico_rtos_get_time();
    time_t utc=time(NULL); if (utc>=1577836800) r->utc=(uint32_t)utc;
    mico_rtos_unlock_mutex(&service_lock);
}
json_object *service_parse(const void *data,size_t n) {
    if (!data || !n || n>8192 || memchr(data,0,n)) return NULL;
    /* This pinned json-c computes string length with strlen, losing embedded
       Unicode NULs. Reject that escape before the parser can truncate it. */
    for(size_t i=0;i+6<=n;i++)if(!memcmp((const char *)data+i,"\\u0000",6))return NULL;
    struct json_tokener *t=json_tokener_new(); if (!t) return NULL;
    json_object *o=json_tokener_parse_ex(t,data,n); int used=t->char_offset;
    bool valid=t->err==json_tokener_success && o && json_object_is_type(o,json_type_object);
    while (used<(int)n && isspace(((const unsigned char *)data)[used])) used++;
    valid=valid && used==(int)n;json_tokener_free(t);
    if (!valid) { if(o)json_object_put(o);return NULL; } return o;
}
static json_object *result(const char *id,const char *code,json_object *data) {
    json_object *o=json_object_new_object(); if (!o) { if(data)json_object_put(data);return NULL; }
    if(id)string(o,"request_id",id);string(o,"code",code);
    const char *message=!strcmp(code,"ok")?"操作成功":!strcmp(code,"accepted")?"请求已接受":!strcmp(code,"busy")?"设备忙":!strcmp(code,"unsupported")?"不支持此功能":!strcmp(code,"conflict")?"请求编号冲突":!strcmp(code,"storage_error")?"配置保存失败":"参数无效";
    string(o,"message",message);json_build_object_add(o,"data",data?data:json_object_new_object());return o;
}
static bool uint_param(json_object *p,const char *k,int *v,int max) {
    json_object *o=json_object_object_get(p,k); if(!o)return true;
    if(!json_object_is_type(o,json_type_int))return false;*v=json_object_get_int(o);return *v>=0 && *v<=max;
}
static json_object *logs(json_object *p,const char **code) {
    int after=0,limit=8;
    if(!uint_param(p,"after",&after,2147483647) || !uint_param(p,"limit",&limit,8) || !limit){*code="invalid_params";return NULL;}
    json_object *o=json_object_new_object(),*a=json_object_new_array();
    mico_rtos_lock_mutex(&service_lock);
    uint32_t first=sequence>=LOG_COUNT?sequence-LOG_COUNT+1:1, next=(uint32_t)after;
    json_build_object_add(o,"overwritten",json_object_new_boolean(after && (uint32_t)after<first-1));
    for(uint32_t i=first;i<=sequence && limit;i++) if(i>(uint32_t)after) {
        const service_record_t *r=&records[(i-1)%LOG_COUNT];json_object *v=json_object_new_object();
        json_build_object_add(v,"sequence",json_object_new_int(r->sequence));json_build_object_add(v,"uptime_ms",json_object_new_int(r->uptime));
        if(r->utc)json_build_object_add(v,"utc",json_object_new_int(r->utc));
        char text[40];const char *event=log_event_text(r,text);
        string(v,"level",log_levels[r->level]);string(v,"module",log_modules[r->module]);string(v,"event",event);string(v,"message",event);
        json_build_array_add(a,v);next=i;limit--;
    }
    json_build_object_add(o,"next",json_object_new_int(next));json_build_object_add(o,"latest",json_object_new_int(sequence));
    mico_rtos_unlock_mutex(&service_lock);json_build_object_add(o,"records",a);return o;
}
#include "ota/update.inc"
json_object *service_command(json_object *req) {
    const char *code="invalid_params",*id=NULL,*action=NULL;
    if(!ready || !req || !json_object_is_type(req,json_type_object))return result(NULL,code,NULL);
    json_object *a=json_object_object_get(req,"action"),*p=json_object_object_get(req,"params"),*rid=json_object_object_get(req,"request_id");
    if(rid){if(!json_object_is_type(rid,json_type_string) || json_object_get_string_len(rid)!=(int)strlen(json_object_get_string(rid)) || !identifier(json_object_get_string(rid),65))return result(NULL,code,NULL);id=json_object_get_string(rid);}
    if(!a || !json_object_is_type(a,json_type_string) || json_object_get_string_len(a)!=(int)strlen(json_object_get_string(a)) || !identifier(json_object_get_string(a),48) || !p || !json_object_is_type(p,json_type_object))return result(id,code,NULL);
    json_object_object_foreach(req,key,value){(void)value;if(strcmp(key,"action")&&strcmp(key,"params")&&strcmp(key,"request_id"))return result(id,code,NULL);}
    action=json_object_get_string(a);
    if(!strcmp(action,"info.get")){
        const char *status_code="ok";json_object *status=service_product("status.get",p,&status_code);
        if(!status)return result(id,status_code,NULL);
        json_object *info=json_object_new_object();
        const char *keys[]={"model","device_id","version"};
        for(unsigned i=0;i<3;i++){json_object *v=json_object_object_get(status,keys[i]);if(v)json_build_object_add(info,keys[i],json_object_get(v));}
        phicomm_variant_json(info);json_object_put(status);return result(id,"ok",info);
    }
#ifdef PHICOMM_DEVICE_OTA
    if(!strcmp(action,"ota.cancel"))return result(id,ota_job_cancel()?"accepted":"busy",NULL);
    if(!strcmp(action,"ota.status.get") || (ota_job_active() && !strcmp(action,"status.get")))return result(id,"ok",ota_job_status());
    if(ota_job_active() && !strcmp(action,"capabilities.get")){json_object *o=json_object_new_object(),*a=json_object_new_array();const char *names[]={"status.get","ota.sources.get","ota.check.get","ota.notes.get","ota.status.get","ota.cancel"};for(unsigned i=0;i<6;i++)json_build_array_add(a,json_object_new_string(names[i]));json_build_object_add(o,"actions",a);phicomm_variant_json(o);return result(id,"ok",o);}
    if(ota_job_active() && strcmp(action,"ota.check.get") && strcmp(action,"ota.sources.get") && strcmp(action,"ota.notes.get"))return result(id,"busy",NULL);
#endif
    bool query=strlen(action)>4 && !strcmp(action+strlen(action)-4,".get");
    /* Receipt stores only control requests. Configuration requests can contain
       passwords and are never retained in the replay cache. */
    bool cache=id && !query && strncmp(action,"config.",7) && strncmp(action,"wifi.",5) && strncmp(action,"ota.",4);
    char *canonical=NULL;
    if(cache){canonical=receipt_json_copy(req);if(!canonical)return result(id,"busy",NULL);}
    mico_rtos_lock_mutex(&command_lock);
    if(cache)for(unsigned i=0;i<8;i++)if(!strcmp(receipts[i].id,id)){
        json_object *out=!strcmp(receipts[i].request,canonical)?(receipts[i].response?service_parse(receipts[i].response,strlen(receipts[i].response)):result(id,"busy",NULL)):result(id,"conflict",NULL);
        mico_rtos_unlock_mutex(&command_lock);free(canonical);return out;
    }
    /* Serialization covers mutation + receipt insertion across HTTP/MQTT. */
    json_object *data=NULL;
    if(!strcmp(action,"capabilities.get")){data=service_product_capabilities();update_capabilities(data);phicomm_variant_json(data);code="ok";}
    else if(!strcmp(action,"logs.get")){
        /* logs() locks the same mutex: queries do not hold the dispatch lock. */
        mico_rtos_unlock_mutex(&command_lock);data=logs(p,&code);if(data)code="ok";return result(id,code,data);
    } else { bool handled=false;data=update_command(action,p,&code,&handled);if(!handled)data=automation_command(action,p,&code,&handled);if(!handled)data=service_product(action,p,&code); }
    json_object *out=result(id,code,data);
    if(cache){receipt_t *r=&receipts[receipt_next++%8];free(r->request);free(r->response);memset(r,0,sizeof(*r));strcpy(r->id,id);r->request=canonical;canonical=NULL;r->response=receipt_json_copy(out);/* Keep the ID/request if response allocation fails: the mutation already ran. */}
    mico_rtos_unlock_mutex(&command_lock);free(canonical);
    if(!query)service_event(!strcmp(code,"accepted")||!strcmp(code,"ok")?"info":"warn","command",!strcmp(code,"accepted")||!strcmp(code,"ok")?"accepted":"rejected");
    return out;
}
json_object *service_query(const char *action) {
    json_object *req=json_object_new_object();string(req,"action",action);json_build_object_add(req,"params",json_object_new_object());json_object *r=service_command(req);json_object_put(req);return r;
}
