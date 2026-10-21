#include "../common/mqtt/diagnostics.h"
#include "../common/ota/mode.h"
#include "../common/watchdog.h"
#include "../common/service.h"
#include "m1.h"
#include "../common/ota/signed_ota.h"
#include "web_data.h"
#include "../common/firmware_variant.h"
#include "../common/http_cors.h"
#include "favicon.h"
#include <ctype.h>
#include <errno.h>
#include "../common/device_status.h"
#include <limits.h>
#include "../common/httpd/product.h"


#define HTTP_PENDING_CLIENTS 1u
#define HTTP_TCP_CLIENT_LIMIT 4
#define http_diag_t ph_http_diagnostics
#define http_diag_snapshot ph_http_snapshot
static ph_http_product product;
static const char *query_action;
static enum { QUERY, COMMAND, LIGHT, UPLOAD, DIRECT } operation;
static bool upload_open, upload_pending;
static bool response(ph_http_exchange *x,unsigned status,const char *type,const void *body,size_t length,bool gzip)
{
    x->response.status=status;x->response.type=type;x->response.body=body;
    x->response.length=length;x->response.gzip=gzip;return true;
}
static void str(json_object *o, const char *key, const char *value)
{ json_build_object_add(o, key, json_object_new_string(value)); }
static void integer(json_object *o, const char *key, int value)
{ json_build_object_add(o, key, json_object_new_int(value)); }
static void json_bool(json_object *o, const char *key, bool value)
{ json_build_object_add(o, key, json_object_new_boolean(value)); }

static __attribute__((noinline)) json_object *m1_status_full(void)
{
    device_status_t snapshot; device_status_full_t full;
    m1_status_capture(&snapshot,&full);
    return device_status_encode(&snapshot,&full);
}
static __attribute__((noinline)) json_object *m1_status_live(void)
{
    device_status_t snapshot; m1_status_capture(&snapshot,NULL);
    return device_status_encode(&snapshot,NULL);
}
static __attribute__((noinline)) json_object *m1_status_json_view(bool live)
{ return live ? m1_status_live() : m1_status_full(); }
json_object *m1_status_json(void) { return m1_status_json_view(false); }
json_object *m1_status_live_json(void) { return m1_status_json_view(true); }

static void memory_response(ph_http_exchange *x)
{
    /* Keep diagnostics usable when building the full status JSON fails.
     * Authentication is applied by handle() before reaching this route. */
    const micoMemInfo_t *memory = MicoGetMemoryInfo();
    if (!memory) {
        static const char unavailable[] = "{\"code\":\"unavailable\"}";
        response(x, 503, "application/json", unavailable, sizeof(unavailable) - 1, false);
        return;
    }
    micoMemInfo_t snapshot = *memory;
    service_memory_state_t budget; service_memory_snapshot(&budget);
    /* Request-owned diagnostics scratch; guard failure preserves the small
     * allocation-free base response. Never increase permanent scalar RAM. */
    size_t capacity=2304;product.body=snapshot.free_memory>=SERVICE_MEMORY_RESERVE+capacity+32?json_heap_malloc(capacity):NULL;
    char *body=product.body?product.body:product.scalar;
    if(!product.body)capacity=sizeof(product.scalar);
    int length = snprintf(body, capacity,
        "{\"code\":\"ok\",\"data\":{\"free_memory\":%d,\"allocated_memory\":%d,"
        "\"free_memory_chunks\":%d,\"uptime_ms\":%lu,\"memory_reserve\":%u,"
        "\"memory_reserved\":%lu,\"memory_active\":%lu,\"memory_waiting\":%lu,"
        "\"memory_denied\":%lu,\"minimum_sampled_free\":%lu,"
        "\"json_live_blocks\":%lu,\"json_peak_blocks\":%lu,\"json_alloc_failures\":%lu,\"json_guard_denied\":%lu}}",
        snapshot.free_memory, snapshot.allocted_memory, snapshot.num_of_chunks,
        (unsigned long)mico_rtos_get_time(), SERVICE_MEMORY_RESERVE,
        (unsigned long)budget.reserved, (unsigned long)budget.active,
        (unsigned long)budget.waiting, (unsigned long)budget.denied,
        (unsigned long)budget.minimum_free,
        (unsigned long)budget.json_live, (unsigned long)budget.json_peak,
        (unsigned long)budget.json_failed, (unsigned long)budget.json_denied);
    ph_http_diagnostics d=ph_http_snapshot();
    if(length>0)length=ph_http_diag_append(body,capacity,length,&d);
    if(length>0)length=ph_mqtt_diag_append(body,capacity,length,product.body!=NULL);
    if (length > 0 && (size_t)length < capacity)
        response(x, 200, "application/json", body, (size_t)length, false);
}


static ph_http_action prepare(ph_http_exchange *x)
{
    if(upload_pending) {
        upload_pending=false;
        bool rescue=!strcmp(x->request->path,"/api/v1/ota/upload-unsigned");
        if(!(rescue?m1_ota_begin_unsigned(x->request->length):m1_ota_begin(x->request->length))) {
            ph_http_message(&product,x,400,"Invalid OTA size or busy");return PH_HTTP_REPLY;
        }
        upload_open=true;x->body_idle_ms=15000;x->body_total_ms=300000;return PH_HTTP_READ_BODY;
    }
    size_t input=operation==QUERY?0:x->request->length;
    ph_http_action next=ph_http_claim(&product,x,input,operation!=QUERY);
    if(next==PH_HTTP_REPLY && product.claim && operation==QUERY)
        ph_http_json(&product,x,service_query(query_action));
    return next;
}
static ph_http_action begin(ph_http_exchange *x)
{
    ph_http_product_init(&product,x);operation=DIRECT;upload_open=false;
    const ph_http_request *r=x->request;
    x->response.cors=phicomm_cors_headers(r->origin);
    if(!strcmp(r->method,"OPTIONS")) { response(x,*x->response.cors?200:403,"text/plain","",0,false);return PH_HTTP_REPLY; }
    m1_config_t cfg;m1_config_get(&cfg);
    if(!ph_http_basic(r->authorization,"admin",cfg.web_password)) {
        ph_http_message(&product,x,401,"Authentication required");x->response.realm="M1";return PH_HTTP_REPLY;
    }
    if(strncmp(r->path,"/api/v1/",8) && strcmp(r->path,"/") && strcmp(r->path,"/index.html") && strcmp(r->path,"/favicon.ico")) {
        ph_http_message(&product,x,404,"Not found");return PH_HTTP_REPLY;
    }
    if(!strcmp(r->method,"GET")) {
        if(!strcmp(r->path,"/api/v1/memory")) { memory_response(x);return PH_HTTP_REPLY; }
        query_action=!strcmp(r->path,"/api/v1/info")?"info.get":!strcmp(r->path,"/api/v1/status")?"status.get":
          !strcmp(r->path,"/api/v1/capabilities")?"capabilities.get":!strcmp(r->path,"/api/v1/config")?"config.get":
          !strcmp(r->path,"/api/v1/logs")?"logs.get":!strcmp(r->path,"/api/v1/schedules")?"schedules.get":NULL;
        if(query_action) { operation=QUERY;return prepare(x); }
        const char *tag=NULL;
        if(!strcmp(r->path,"/") || !strcmp(r->path,"/index.html")) {
            response(x,200,"text/html; charset=utf-8",m1_web_data,sizeof(m1_web_data),true);tag=M1_WEB_ETAG;
        } else if(!strcmp(r->path,"/favicon.ico")) {
            response(x,200,"image/x-icon",m1_favicon_data,sizeof(m1_favicon_data),false);tag=M1_FAVICON_ETAG;
        }
        if(tag) {
            x->response.etag=tag;x->response.private_cache=true;x->response.cors="";
            if(!strcmp(tag,r->if_none_match) || (!strncmp(r->if_none_match,"W/",2) && !strcmp(tag,r->if_none_match+2)))x->response.status=304;
            return PH_HTTP_REPLY;
        }
    }
    if(!strcmp(r->method,"POST") && !strcmp(r->path,"/api/v1/command"))operation=COMMAND;

    if(operation==COMMAND || operation==LIGHT) {
        if(ota_job_active() && (operation==LIGHT || r->length>1024)) {
            ph_http_message(&product,x,409,"OTA in progress");return PH_HTTP_REPLY;
        }
        if(r->length>M1_JSON_LIMIT) { ph_http_message(&product,x,400,"Invalid JSON size");return PH_HTTP_REPLY; }
        return prepare(x);
    }
    if(!strcmp(r->method,"POST") && (!strcmp(r->path,"/api/v1/ota/upload") || !strcmp(r->path,"/api/v1/ota/upload-unsigned"))) {
        if(!r->have_length || !r->length) {
            ph_http_message(&product,x,400,"Invalid OTA size or busy");return PH_HTTP_REPLY;
        }
        /* Verify reduced listener admission before synchronous flash setup. */
        operation=UPLOAD;upload_pending=true;return PH_HTTP_WAIT;
    }
    ph_http_message(&product,x,404,"Not found");return PH_HTTP_REPLY;
}
static bool body_chunk(ph_http_exchange *x,const void *data,size_t n)
{
    if(operation==UPLOAD) {
        if(m1_ota_write(data,n))return true;
        /* The product writer has already aborted/released this upload. */
        upload_open=false;
        ph_http_message(&product,x,400,"OTA rejected; see /ota for details");return false;
    }
    return ph_http_body_copy(&product,data,n);
}
static void complete(ph_http_exchange *x)
{
    if(operation==UPLOAD) {
        bool ok=m1_ota_finish();upload_open=false;
        ph_http_message(&product,x,ok?202:400,ok?"OTA verified; rebooting":"OTA rejected; see /ota for details");return;
    }
    json_object *request=ph_http_body_json(&product,x);
    json_object *reply=!ph_http_graph_failed(&product)?service_command(request):NULL;
    /* The reply owns its references; retire input before growing its wire. */
    json_object_put(request);
    ph_http_json(&product,x,reply);
}
static void release(ph_http_exchange *x,bool sent)
{
    (void)x;(void)sent;if(upload_open)m1_ota_abort("Upload interrupted");upload_open=false;upload_pending=false;
    ph_http_product_release(&product);
}
static unsigned admission(void) { ota_job_poll();return (upload_pending || ota_job_active())?2:HTTP_TCP_CLIENT_LIMIT; }
static const ph_http_app app={begin,prepare,body_chunk,complete,release,admission};
OSStatus m1_http_start(void) { return ph_http_start(&app,HTTP_TCP_CLIENT_LIMIT); }
