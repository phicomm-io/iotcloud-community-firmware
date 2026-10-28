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
#include "../common/backup.h"


#define HTTP_PENDING_CLIENTS 1u
#define HTTP_TCP_CLIENT_LIMIT 4
#define http_diag_t ph_http_diagnostics
#define http_diag_snapshot ph_http_snapshot
static ph_http_product product;
static const char *query_action;
static enum { QUERY, AUTH, COMMAND, LIGHT, UPLOAD, MCU, MCU_BULK, DIRECT , BACKUP, RESTORE } operation;
static bool upload_open, upload_pending;
static bool response(ph_http_exchange *x,unsigned status,const char *type,const void *body,size_t length,bool gzip)
{
    x->response.status=status;x->response.type=type;x->response.body=body;
    x->response.length=length;x->response.gzip=gzip;return true;
}
#define HTTP_AUTH_MODEL "m1"
#define HTTP_AUTH_ID m1_id
#define HTTP_AUTH_VERSION FIRMWARE_VERSION
#include "../common/httpd/local_auth.inc"
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



/* Stock 11.x has a ten-second transaction timer. Buffer before begin so
 * network round trips cannot consume that UART-only transfer window. */
static bool mcu_bulk(const uint8_t *image,size_t n)
{
    uint32_t sp,pc;uint8_t f[262]={0x23,0x12},ack[20];
    if(n<164 || n>32768)return false;
    memcpy(&sp,image,4);memcpy(&pc,image+4,4);
    if(sp<=0x20000000u || sp>0x20008000u || (sp&7) || !(pc&1) ||
       (pc&~1u)<0x4000 || (pc&~1u)>0x4000+n-2)return false;
    f[2]=n/256;f[3]=n%256;f[11]=0x21;
    uint32_t start=mico_rtos_get_time();
    if(!m1_mcu_exchange(f,12,ack) || ack[1]!=0x14 || ack[2] || ack[3])goto cancel;
    for(size_t offset=0,index=1;offset<=n;offset+=256,index++) {
        size_t take=n-offset;if(take>256)take=256;
        uint16_t crc=0;
        for(size_t i=0;i<take;i++)for(int bit=7;bit>=0;bit--){
            bool high=crc&0x8000;crc=(uint16_t)((crc<<1)|((image[offset+i]>>bit)&1));
            if(high)crc^=0x1021;
        }
        f[1]=0x13;f[2]=index>>8;f[3]=index;memcpy(f+4,image+offset,take);
        f[4+take]=crc>>8;f[5+take]=crc;
        if((uint32_t)(mico_rtos_get_time()-start)>8500 ||
           !m1_mcu_exchange(f,take+6,ack) || ack[1]!=0x14 ||
           ack[2]!=f[2] || ack[3]!=f[3] || ack[4]!=1)goto cancel;
        if(take<256)break;
    }
    /* UART owner must consume the separate receive-complete notification. */
    mico_thread_msleep(50);
    memset(f,0,12);f[0]=0x23;f[1]=0x16;f[11]=0x21;
    if((uint32_t)(mico_rtos_get_time()-start)>9000 || !m1_mcu_exchange(f,12,ack))goto cancel;
    return true; /* UART transmit receipt; runtime version is still required. */
cancel:
    memset(f,0,12);f[0]=0x23;f[1]=0x16;f[2]=1;f[11]=0x21;
    m1_mcu_exchange(f,12,ack);return false;
}

static ph_http_action prepare(ph_http_exchange *x)
{
    if(operation==MCU_BULK) {
        product.capacity=x->request->length;
        product.body=json_heap_malloc(product.capacity+1);
        if(!product.body){ph_http_busy(x);return PH_HTTP_REPLY;}
        return PH_HTTP_READ_BODY;
    }
    if(upload_pending) {
        upload_pending=false;
        bool rescue=!strcmp(x->request->path,"/api/v1/ota/upload-unsigned");
        if(!(rescue?m1_ota_begin_unsigned(x->request->length):m1_ota_begin(x->request->length))) {
            ph_http_message(&product,x,400,"Invalid OTA size or busy");return PH_HTTP_REPLY;
        }
        upload_open=true;x->body_idle_ms=15000;x->body_total_ms=300000;return PH_HTTP_READ_BODY;
    }
    size_t input=operation==BACKUP?device_backup_size():operation==QUERY?0:x->request->length;
    ph_http_action next=ph_http_claim(&product,x,input,operation!=QUERY);
    if(next==PH_HTTP_READ_BODY && operation==BACKUP){
        if(device_backup_export(product.body,input))response(x,200,"application/octet-stream",product.body,input,false);
        else ph_http_message(&product,x,409,"backup_unavailable");
        return PH_HTTP_REPLY;
    }
    if(next==PH_HTTP_REPLY && product.claim && operation==QUERY)
        ph_http_query(&product,x,query_action);
    return next;
}
static ph_http_action begin(ph_http_exchange *x)
{
    ph_http_product_init(&product,x);operation=DIRECT;upload_open=false;
    const ph_http_request *r=x->request;
    x->response.cors=phicomm_cors_headers(r->origin);
    if(!strcmp(r->method,"OPTIONS")) { response(x,*x->response.cors?200:403,"text/plain","",0,false);return PH_HTTP_REPLY; }
    if(!strcmp(r->method,"GET") && !strcmp(r->path,"/api/v1/info")){
        http_auth_info(&product,x);return PH_HTTP_REPLY;
    }
    if(http_auth_route(r->path)){
        if(strcmp(r->method,"POST")||!r->length||r->length>256){http_auth_reply(&product,x,400,"invalid_request",NULL);return PH_HTTP_REPLY;}
        operation=AUTH;return prepare(x);
    }
    bool asset=!strcmp(r->path,"/")||!strcmp(r->path,"/index.html")||!strcmp(r->path,"/favicon.ico");
    if(!asset && !http_auth_authorized(r->authorization)){
        http_auth_reply(&product,x,401,"unauthorized",NULL);return PH_HTTP_REPLY;
    }
    if(strncmp(r->path,"/api/v1/",8) && strcmp(r->path,"/") && strcmp(r->path,"/index.html") && strcmp(r->path,"/favicon.ico")) {
        ph_http_message(&product,x,404,"Not found");return PH_HTTP_REPLY;
    }
    if(!strcmp(r->path,"/api/v1/config/backup") || !strcmp(r->path,"/api/v1/config/restore")) {
        if(!ph_auth_password_is_set(device_local_auth())){ph_http_message(&product,x,403,"set_web_password_first");return PH_HTTP_REPLY;}
        if(!strcmp(r->method,"GET") && !strcmp(r->path,"/api/v1/config/backup")){operation=BACKUP;return prepare(x);}
        if(!strcmp(r->method,"POST") && !strcmp(r->path,"/api/v1/config/restore")){
            if(!r->have_length || r->length!=device_backup_size()){ph_http_message(&product,x,400,"invalid_backup");return PH_HTTP_REPLY;}
            operation=RESTORE;return prepare(x);
        }
    }
    if(!strcmp(r->method,"GET")) {
        if(!strcmp(r->path,"/api/v1/memory")) { memory_response(x);return PH_HTTP_REPLY; }
        query_action=!strcmp(r->path,"/api/v1/info")?"info.get":!strcmp(r->path,"/api/v1/status")?"status.get":
          !strcmp(r->path,"/api/v1/capabilities")?"capabilities.get":!strcmp(r->path,"/api/v1/config")?"config.get":
          !strcmp(r->path,"/api/v1/sensor_details")?"sensor_details.get":!strcmp(r->path,"/api/v1/logs")?"logs.get":!strcmp(r->path,"/api/v1/schedules")?"schedules.get":NULL;
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
    if(!strcmp(r->method,"POST") && !strcmp(r->path,"/api/v1/mcu/ota")) {
        if(!r->have_length || r->length<164 || r->length>32768){ph_http_message(&product,x,400,"Invalid MCU image");return PH_HTTP_REPLY;}
        mico_rtos_lock_mutex(&m1_lock);
        bool busy=m1_state.mcu_updating || m1_state.ota_active || m1_state.reboot_pending ||
            m1_state.wifi_trial_active || !m1_state.uart_ready;
        mico_rtos_unlock_mutex(&m1_lock);
        if(busy){ph_http_message(&product,x,409,"MCU busy");return PH_HTTP_REPLY;}
        operation=MCU_BULK;x->body_idle_ms=5000;x->body_total_ms=30000;
        return prepare(x);
    }
    if(!strcmp(r->method,"POST") && !strcmp(r->path,"/api/v1/mcu/exchange")) {
        if(!r->have_length || r->length<6 || r->length>262) {
            ph_http_message(&product,x,400,"Invalid MCU frame size");return PH_HTTP_REPLY;
        }
        operation=MCU;x->body_idle_ms=5000;x->body_total_ms=5000;
        return prepare(x);
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
    if(operation==AUTH){http_auth_complete(&product,x);return;}
    if(operation==UPLOAD) {
        bool ok=m1_ota_finish();upload_open=false;
        ph_http_message(&product,x,ok?202:400,ok?"OTA verified; rebooting":"OTA rejected; see /ota for details");return;
    }
    if(operation==MCU_BULK) {
        bool ok=product.used==x->request->length && mcu_bulk((const uint8_t *)product.body,product.used);
        json_heap_free(product.body);product.body=NULL;
        ph_http_message(&product,x,ok?202:409,ok?"MCU commit sent; verify runtime version":"MCU transfer failed; not confirmed");return;
    }
    if(operation==MCU) {
        uint8_t reply[20];
        bool ok=product.used==x->request->length &&
            m1_mcu_exchange((const uint8_t *)product.body,product.used,reply);
        if(ok) {
            memcpy(product.scalar,reply,sizeof(reply));
            response(x,200,"application/octet-stream",product.scalar,sizeof(reply),false);
        } else ph_http_message(&product,x,409,"MCU busy or response timeout");
        return;
    }
    if(operation==RESTORE){const char *code=device_backup_import(product.body,product.used);ph_http_message(&product,x,!strcmp(code,"accepted")?202:!strcmp(code,"busy")?409:400,code);return;}
    json_object *request=ph_http_body_json(&product,x);
    ph_http_command(&product,x,request);
    /* Wire owns all text, including any borrowed request_id. */
    json_object_put(request);
}
static void release(ph_http_exchange *x,bool sent)
{
    (void)x;(void)sent;if(upload_open)m1_ota_abort("Upload interrupted");upload_open=false;upload_pending=false;
    ph_http_product_release(&product);
}
static unsigned admission(void) { ota_job_poll();return (upload_pending || ota_job_active())?2:HTTP_TCP_CLIENT_LIMIT; }
static const ph_http_app app={begin,prepare,body_chunk,complete,release,admission};
OSStatus m1_http_start(void) { return ph_http_start(&app,HTTP_TCP_CLIENT_LIMIT); }
