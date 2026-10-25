#include "service.h"
#include "backup.h"
__attribute__((weak)) bool device_config_recovery(void) { return false; }
__attribute__((weak)) void device_config_recovery_set(bool recovery) { (void)recovery; }
#include "json_owned.h"
#include "status_view.h"
#include "printbuf.h"
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
#include "json_heap.h"
#include "sha.h"
#define LOG_COUNT 64
/* Fixed vocabulary events never accept credentials, payloads or URLs. */
typedef struct { uint32_t sequence, uptime, utc; int32_t argument; uint8_t level,module,event,reserved; } service_record_t;
#include "log_dictionary.h"
static service_record_t records[LOG_COUNT];
static uint32_t sequence;
static mico_mutex_t service_lock, command_lock;
static bool ready, service_lock_ready;
static mico_mutex_t json_lock;
static bool json_ready;
static service_memory_state_t memory_state = {.minimum_free = UINT32_MAX};
static unsigned json_owner, json_generation;
/* One lease already excludes competing graph producers. The allocator mutex
 * protects this attribution record; the hook never takes a service/core lock. */
static struct { unsigned token, failed; void *owner; } json_scope;
static uint32_t service_free_memory(void) {
#if defined(MOC) && MOC == 1
    const micoMemInfo_t *m = MicoGetMemoryInfo();
    return m && m->free_memory > 0 ? (uint32_t)m->free_memory : 0;
#else
    return UINT32_MAX; /* Host simulators use the policy's injectable unit tests. */
#endif
}
void json_heap_lock(void) { if (json_ready) mico_rtos_lock_mutex(&json_lock); }
void json_heap_unlock(void) { if (json_ready) mico_rtos_unlock_mutex(&json_lock); }
int json_heap_permit(size_t bytes) {
    uint32_t free_bytes = service_free_memory();
    /* Include allocator metadata/alignment. SDK/network allocations can still
     * race this check; allocation failure remains a handled result. */
    return free_bytes >= SERVICE_MEMORY_RESERVE + 32u &&
        bytes <= free_bytes - SERVICE_MEMORY_RESERVE - 32u;
}
uint64_t service_memory_failures(void) {
    json_heap_stats_t s; json_heap_snapshot(&s);
    return (uint64_t)s.allocation_failures + s.guard_denied;
}
void json_heap_failure(unsigned reason) {
    /* Called with json_lock held, including overflow/guard failures. */
    if (json_scope.owner && mico_rtos_is_current_thread(json_scope.owner))
        json_scope.failed |= reason;
}
bool service_json_scope_begin(unsigned token, void *known_owner) {
    if (!ready || !token || !known_owner || !mico_rtos_is_current_thread(known_owner)) return false;
    mico_rtos_lock_mutex(&service_lock);
    json_heap_lock();
    bool valid = json_owner == token && !json_scope.token;
    if (valid) { json_scope.token=token;json_scope.owner=known_owner;json_scope.failed=0; }
    json_heap_unlock();
    mico_rtos_unlock_mutex(&service_lock);
    return valid;
}
unsigned service_json_scope_failed(unsigned token) {
    json_heap_lock();
    unsigned failed = token && json_scope.token==token && json_scope.owner &&
        mico_rtos_is_current_thread(json_scope.owner) ? json_scope.failed : 4u;
    json_heap_unlock();
    return failed;
}
unsigned service_json_scope_end(unsigned token) {
    json_heap_lock();
    unsigned failed=4u;
    if (token && json_scope.token==token && json_scope.owner && mico_rtos_is_current_thread(json_scope.owner)) {
        failed=json_scope.failed;memset(&json_scope,0,sizeof(json_scope));
    }
    json_heap_unlock();
    return failed;
}
bool service_caller_graph_failed(uint64_t global_before) {
    json_heap_lock();
    bool scoped=json_scope.owner && mico_rtos_is_current_thread(json_scope.owner);
    bool failed=json_scope.failed!=0;
    json_heap_unlock();
    return scoped ? failed : service_memory_failures()!=global_before;
}
unsigned service_memory_claim(size_t input_bytes) {
    if (!ready) return 0;
    mico_rtos_lock_mutex(&service_lock);
    unsigned claim = memory_budget_claim(&memory_state, service_free_memory(), input_bytes);
    mico_rtos_unlock_mutex(&service_lock);
    return claim;
}
unsigned service_json_claim(void) {
    if (!ready) return 0;
    mico_rtos_lock_mutex(&service_lock);
    unsigned token = 0;
    if (!json_owner) {
        if (!++json_generation) ++json_generation;
        token = json_owner = json_generation;
    }
    mico_rtos_unlock_mutex(&service_lock);
    return token;
}
void service_json_release(unsigned token) {
    if (!ready || !token) return;
    mico_rtos_lock_mutex(&service_lock);
    if (json_owner == token) {
        json_heap_lock();
        if (json_scope.token==token) memset(&json_scope,0,sizeof(json_scope));
        json_heap_unlock();
        json_owner = 0;
    }
    mico_rtos_unlock_mutex(&service_lock);
}
void service_memory_release(unsigned claim) {
    if (!ready) return;
    mico_rtos_lock_mutex(&service_lock);
    memory_budget_release(&memory_state, claim);
    mico_rtos_unlock_mutex(&service_lock);
}
void service_memory_waiting(bool enter) {
    if (!ready) return;
    mico_rtos_lock_mutex(&service_lock);
    if (enter) memory_state.waiting++;
    else if (memory_state.waiting) memory_state.waiting--;
    mico_rtos_unlock_mutex(&service_lock);
}
void service_memory_snapshot(service_memory_state_t *out) {
    if (!ready) { memset(out, 0, sizeof(*out)); return; }
    mico_rtos_lock_mutex(&service_lock);
    uint32_t free_bytes = service_free_memory();
    if (free_bytes < memory_state.minimum_free) memory_state.minimum_free = free_bytes;
    *out = memory_state;
    json_heap_stats_t json; json_heap_snapshot(&json);
    out->json_live = json.live_blocks; out->json_peak = json.peak_blocks;
    out->json_failed = json.allocation_failures; out->json_denied = json.guard_denied;
    mico_rtos_unlock_mutex(&service_lock);
}
void service_memory_mqtt(unsigned pending, bool overflow, uint32_t delay_ms) {
    if (!ready) return;
    mico_rtos_lock_mutex(&service_lock);
    memory_state.mqtt_pending=pending;
    if (pending>memory_state.mqtt_peak) memory_state.mqtt_peak=pending;
    if (overflow && memory_state.mqtt_overflow!=UINT32_MAX) memory_state.mqtt_overflow++;
    if (delay_ms>memory_state.mqtt_max_delay) memory_state.mqtt_max_delay=delay_ms;
    mico_rtos_unlock_mutex(&service_lock);
}
/* Retain a fixed digest, never an arbitrarily large control payload. The
 * fingerprint and response preserve the eight-request replay/conflict window. */
typedef struct { char id[65]; unsigned char digest[32]; char *response; } receipt_t;
static receipt_t receipts[8];
static unsigned receipt_next;
static char *receipt_json_copy(json_object *o) {
    const char *wire = o ? json_object_to_json_string(o) : NULL;
    return wire ? json_heap_strdup(wire) : NULL;
}
static void string(json_object *o,const char *k,const char *v) { json_build_object_add(o,k,json_object_new_string(v)); }
void service_init(void) {
    if (ready) return;
    if (!json_ready) json_ready = mico_rtos_init_mutex(&json_lock)==kNoErr;
    if (!json_ready) return;
    if (!service_lock_ready) service_lock_ready = mico_rtos_init_mutex(&service_lock)==kNoErr;
    if (!service_lock_ready) return;
    ready = mico_rtos_init_mutex(&command_lock)==kNoErr;
    if (!ready) return;
    ota_job_init();
    service_event("info","system","boot");
}
bool service_control_begin(void) {
    if (!ready) return false;
    mico_rtos_lock_mutex(&command_lock);
    if (ota_job_active()) { mico_rtos_unlock_mutex(&command_lock); return false; }
    return true;
}
void service_control_end(void) { mico_rtos_unlock_mutex(&command_lock); }
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
typedef struct { const char *id,*action;json_object *params; } service_request;
static bool service_request_decode(json_object *req,service_request *out)
{
    memset(out,0,sizeof(*out));
    if(!ready || !req || !json_object_is_type(req,json_type_object))return false;
    json_object *a=json_object_object_get(req,"action"),*p=json_object_object_get(req,"params"),*rid=json_object_object_get(req,"request_id");
    if(rid){if(!json_object_is_type(rid,json_type_string) || json_object_get_string_len(rid)!=(int)strlen(json_object_get_string(rid)) || !identifier(json_object_get_string(rid),65))return false;out->id=json_object_get_string(rid);}
    if(!a || !json_object_is_type(a,json_type_string) || json_object_get_string_len(a)!=(int)strlen(json_object_get_string(a)) || !identifier(json_object_get_string(a),48) || !p || !json_object_is_type(p,json_type_object))return false;
    json_object_object_foreach(req,key,value){(void)value;if(strcmp(key,"action")&&strcmp(key,"params")&&strcmp(key,"request_id"))return false;}
    out->action=json_object_get_string(a);out->params=p;
    return true;
}
static json_object *service_command_execute(json_object *req) {
    service_request view;
    if(!service_request_decode(req,&view))return result(view.id,"invalid_params",NULL);
    const char *code="invalid_params",*id=view.id,*action=view.action;
    json_object *p=view.params;
    if(!strcmp(action,"info.get")){
        const char *status_code="ok";json_object *status=service_product("info.get",p,&status_code);
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
    if(device_config_recovery() && !query && strcmp(action,"system.reset") && strcmp(action,"system.reboot") && strcmp(action,"wifi.scan"))return result(id,"migration_required",NULL);
    if(device_config_recovery() && !strcmp(action,"system.reset") && !json_object_get_boolean(json_object_object_get(p,"full")))return result(id,"migration_required",NULL);
    /* Receipt stores only control requests. Configuration requests can contain
       passwords and are never retained in the replay cache. */
    bool cache=id && !query && strncmp(action,"config.",7) && strncmp(action,"wifi.",5) && strncmp(action,"ota.",4);
    unsigned char digest[32];
    if(cache){
        const char *canonical=json_object_to_json_string(req);
        if(!canonical)return result(id,"busy",NULL);
        SHA256Context sha;
        if(SHA256Reset(&sha) || SHA256Input(&sha,(const uint8_t *)canonical,strlen(canonical)) ||
           SHA256Result(&sha,digest))return result(id,"busy",NULL);
    }
    mico_rtos_lock_mutex(&command_lock);
    if(cache)for(unsigned i=0;i<8;i++)if(!strcmp(receipts[i].id,id)){
        json_object *out=!memcmp(receipts[i].digest,digest,sizeof(digest))?(receipts[i].response?service_parse(receipts[i].response,strlen(receipts[i].response)):result(id,"busy",NULL)):result(id,"conflict",NULL);
        mico_rtos_unlock_mutex(&command_lock);return out;
    }
    /* Serialization covers mutation + receipt insertion across HTTP/MQTT. */
    json_object *data=NULL;
    if(!strcmp(action,"capabilities.get")){data=service_product_capabilities();update_capabilities(data);phicomm_variant_json(data);code="ok";}
    else if(!strcmp(action,"logs.get")){
        /* logs() locks the same mutex: queries do not hold the dispatch lock. */
        mico_rtos_unlock_mutex(&command_lock);data=logs(p,&code);if(data)code="ok";return result(id,code,data);
    } else { bool handled=false;data=update_command(action,p,&code,&handled);if(!handled)data=automation_command(action,p,&code,&handled);if(!handled)data=service_product(action,p,&code); }
    json_object *out=result(id,code,data);
    if(cache){receipt_t *r=&receipts[receipt_next++%8];json_heap_free(r->response);memset(r,0,sizeof(*r));strcpy(r->id,id);memcpy(r->digest,digest,sizeof(digest));r->response=receipt_json_copy(out);/* Keep the ID/digest if response allocation fails: the mutation already ran. */}
    mico_rtos_unlock_mutex(&command_lock);
    if(!query)service_event(!strcmp(code,"accepted")||!strcmp(code,"ok")?"info":"warn","command",!strcmp(code,"accepted")||!strcmp(code,"ok")?"accepted":"rejected");
    return out;
}
json_object *service_command(json_object *req) {
    uint64_t before = service_memory_failures();
    if (service_caller_graph_failed(before)) return NULL;
    json_object *out = service_command_execute(req);
    /* Failed temporary graphs (e.g. info.get's status) must poison the caller
     * even when the returned envelope itself has no failed descendants. */
    if (service_caller_graph_failed(before)) { if (out) json_object_put(out); return NULL; }
    return out;
}
json_object *service_query(const char *action) {
    uint64_t failures=service_memory_failures();
    json_object *req=json_object_new_object();string(req,"action",action);json_build_object_add(req,"params",json_object_new_object());
    json_object *r=!service_caller_graph_failed(failures)?service_command(req):NULL;json_object_put(req);return r;
}

struct printbuf *service_command_wire(json_object *req)
{
    uint64_t before=service_memory_failures();
    if(service_caller_graph_failed(before))return NULL;
    service_request view;bool live=false;
    struct printbuf *wire;
    if(service_request_decode(req,&view) && !strcmp(view.action,"status.get") &&
       !ota_job_active() && status_view_params(view.params,&live)) {
        mico_rtos_lock_mutex(&command_lock);
        wire=service_product_status_wire(live,true,view.id);
        mico_rtos_unlock_mutex(&command_lock);
    } else wire=json_owned_to_wire(service_command(req));
    if(service_caller_graph_failed(before)) { if(wire)printbuf_free(wire);return NULL; }
    return wire;
}
/* Optional product hook: only the admitted MQTT dispatcher reaches it. */
extern json_object *service_product_mqtt(const char *,json_object *,const char **) __attribute__((weak));
struct printbuf *service_mqtt_command_wire(json_object *req) {
    service_request view;
    if(!service_request_decode(req,&view) || strcmp(view.action,"sensor.session.set"))return service_command_wire(req);
    uint64_t before=service_memory_failures();
    if(service_caller_graph_failed(before))return NULL;
    const char *code="unsupported";json_object *data=NULL;
    mico_rtos_lock_mutex(&command_lock);
    if(ota_job_active())code="busy";
    else if(service_product_mqtt)data=service_product_mqtt(view.action,view.params,&code);
    json_object *out=result(view.id,code,data);
    mico_rtos_unlock_mutex(&command_lock);
    struct printbuf *wire=json_owned_to_wire(out);
    if(service_caller_graph_failed(before)){if(wire)printbuf_free(wire);return NULL;}
    return wire;
}
struct printbuf *service_query_wire(const char *action)
{
    if(!ready || strcmp(action,"status.get") || ota_job_active())return json_owned_to_wire(service_query(action));
    uint64_t before=service_memory_failures();
    if(service_caller_graph_failed(before))return NULL;
    mico_rtos_lock_mutex(&command_lock);
    struct printbuf *wire=service_product_status_wire(false,true,NULL);
    mico_rtos_unlock_mutex(&command_lock);
    if(service_caller_graph_failed(before)) { if(wire)printbuf_free(wire);return NULL; }
    return wire;
}
