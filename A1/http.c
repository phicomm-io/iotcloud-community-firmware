#include "../common/mqtt/diagnostics.h"
#include "../common/ota/mode.h"
#include "../common/watchdog.h"
#include "../common/service.h"
#include "a1.h"
#include "filter.h"
#include "../common/ota/signed_ota.h"
#include "web_data.h"
#include "../common/firmware_variant_defs.h"
#include "../common/http_cors.h"
#include "favicon.h"
#include <ctype.h>
#include <errno.h>
#include "../common/device_status.h"
#include <limits.h>
#include "../common/httpd/product.h"
#include "../common/backup.h"


#define HTTP_PENDING_CLIENTS PH_HTTP_WAIT_SLOTS
#define HTTP_TCP_CLIENT_LIMIT 3
#define http_diag_t ph_http_diagnostics
#define http_diag_snapshot ph_http_snapshot
static ph_http_product product;
static const char *query_action;
static enum { QUERY, AUTH, COMMAND, LIGHT, UPLOAD, DIRECT , BACKUP, RESTORE } operation;
static bool upload_open, upload_pending;
static bool response(ph_http_exchange *x,unsigned status,const char *type,const void *body,size_t length,bool gzip)
{
    x->response.status=status;x->response.type=type;x->response.body=body;
    x->response.length=length;x->response.gzip=gzip;return true;
}
#define HTTP_AUTH_MODEL "a1"
#define HTTP_AUTH_ID a1_id
#define HTTP_AUTH_VERSION FIRMWARE_VERSION
#include "../common/httpd/local_auth.inc"
#include "../common/httpd/memory_mp.inc"

static void light_command(ph_http_exchange *x,const ph_value *body)
{
 ph_value slot,v;int64_t n;bool adjacent=false;
 if(!phv_keys(body,"|slot||neighbors|")||!phv_get(body,"slot",&slot)||!phv_int(&slot,&n)||n<0||n>32){ph_http_message(&product,x,400,"slot must be 0 (exit) or 1..32");return;}
 if(phv_get(body,"neighbors",&v)&&!phv_bool(&v,&adjacent)){ph_http_message(&product,x,400,"neighbors must be boolean");return;}
 mico_rtos_lock_mutex(&a1_lock);bool busy=a1_state.ota_active||a1_state.reboot_pending;
 if(!busy){a1_state.light_test_slot=(unsigned)n;a1_state.light_test_neighbors=adjacent;a1_state.light_test_until=n?mico_rtos_get_time()+120000:0;}
 mico_rtos_unlock_mutex(&a1_lock);service_cache_changed(SERVICE_CACHE_STATUS);
 ph_http_message(&product,x,busy?409:200,busy?"Device busy":"Light test updated");
}
static ph_http_action prepare(ph_http_exchange *x)
{
    if(operation!=AUTH && !http_auth_authorized(x->request->authorization)){
        http_auth_reply(&product,x,401,"unauthorized",NULL);return PH_HTTP_REPLY;
    }
    if(upload_pending) {
        upload_pending=false;
        bool rescue=!strcmp(x->request->path,"/api/v1/ota/upload-unsigned");
        if(!(rescue?a1_ota_begin_unsigned(x->request->length):a1_ota_begin(x->request->length))) {
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
          !strcmp(r->path,"/api/v1/logs")?"logs.get":!strcmp(r->path,"/api/v1/schedules")?"schedules.get":NULL;
        if(query_action) { operation=QUERY;return prepare(x); }
        const char *tag=NULL;
        if(!strcmp(r->path,"/") || !strcmp(r->path,"/index.html")) {
            response(x,200,"text/html; charset=utf-8",a1_web_data,sizeof(a1_web_data),true);tag=A1_WEB_ETAG;
        } else if(!strcmp(r->path,"/favicon.ico")) {
            response(x,200,"image/x-icon",a1_favicon_data,sizeof(a1_favicon_data),false);tag=A1_FAVICON_ETAG;
        }
        if(tag) {
            x->response.etag=tag;x->response.private_cache=true;x->response.cors="";
            if(!strcmp(tag,r->if_none_match) || (!strncmp(r->if_none_match,"W/",2) && !strcmp(tag,r->if_none_match+2)))x->response.status=304;
            return PH_HTTP_REPLY;
        }
    }
    if(!strcmp(r->method,"POST") && !strcmp(r->path,"/api/v1/command"))operation=COMMAND;
    if(!strcmp(r->method,"POST") && !strcmp(r->path,"/api/v1/light/test"))operation=LIGHT;
    if(operation==COMMAND || operation==LIGHT) {
        if(ota_job_active() && (operation==LIGHT || r->length>1024)) {
            ph_http_message(&product,x,409,"OTA in progress");return PH_HTTP_REPLY;
        }
        if(r->length>8192u) { ph_http_message(&product,x,400,"Invalid MessagePack size");return PH_HTTP_REPLY; }
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
        if(a1_ota_write(data,n))return true;
        /* The product writer has already aborted/released this upload. */
        upload_open=false;
        ph_http_message(&product,x,400,"OTA rejected; see /ota for details");return false;
    }
    return ph_http_body_copy(&product,data,n);
}
static void complete(ph_http_exchange *x)
{
    if(operation!=AUTH && !http_auth_authorized(x->request->authorization)){
        http_auth_reply(&product,x,401,"unauthorized",NULL);return;
    }
    if(operation==AUTH){http_auth_complete(&product,x);return;}
    if(operation==UPLOAD) {
        bool ok=a1_ota_finish();upload_open=false;
        ph_http_message(&product,x,ok?202:400,ok?"OTA verified; rebooting":"OTA rejected; see /ota for details");return;
    }
    if(operation==RESTORE){const char *code=device_backup_import(product.body,product.used);ph_http_message(&product,x,!strcmp(code,"accepted")?202:!strcmp(code,"busy")?409:400,code);return;}
    if(product.used!=x->request->length){ph_http_message(&product,x,400,"invalid_request");return;}
    if(operation==LIGHT){ph_value request;if(!phv_parse(PH_WIRE_MSGPACK,product.body,product.used,&request)||request.kind!=PHV_MAP)ph_http_message(&product,x,400,"invalid_request");else light_command(x,&request);}
    else ph_http_command_mp(&product,x,product.body,product.used);
    /* Synchronous borrowed parameters have expired; output owns all bytes. */
    ph_wire_free(product.body);product.body=NULL;
}
static void release(ph_http_exchange *x,bool sent)
{
    (void)x;(void)sent;if(upload_open)a1_ota_abort("Upload interrupted");upload_open=false;upload_pending=false;
    ph_http_product_release(&product);
}
static unsigned admission(void) { ota_job_poll();return (upload_pending || ota_job_active())?2:HTTP_TCP_CLIENT_LIMIT; }
static const ph_http_app app={begin,prepare,body_chunk,complete,release,admission};
OSStatus a1_http_start(void) { return ph_http_start(&app,HTTP_TCP_CLIENT_LIMIT); }
