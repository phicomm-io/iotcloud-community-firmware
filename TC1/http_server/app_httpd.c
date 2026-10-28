#include "../../common/mqtt/diagnostics.h"
/**
 ******************************************************************************
 * @file    app_https.c
 * @author  QQ DING
 * @version V1.0.0
 * @date    1-September-2015
 * @brief   The main HTTPD server initialization and wsgi handle.
 ******************************************************************************
 *
 *  The MIT License
 *  Copyright (c) 2016 MXCHIP Inc.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is furnished
 *  to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
 *  IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 ******************************************************************************
 */

/* Product adapter only: parsing, transport and ownership live in common/httpd. */
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mico.h"
#include "app_httpd.h"
#include "auth.h"
#include "main.h"
#include "user_gpio.h"
#include "user_wifi.h"
#include "user_power.h"
#include "http_trace.h"
#include "mqtt_server/user_mqtt_client.h"
#include "ota_server/signed_adapter.h"
#include "../../common/httpd/product.h"
#include "../../common/backup.h"
#include "../../common/http_cors.h"
#include "../../common/firmware_variant.h"
#include "../../common/ota/mode.h"
#include "../../common/device_status.h"
#include <web_data.c>
static ph_http_product product;
static const char *query_action;
static enum { DIRECT, QUERY, COMMAND, UPLOAD, BACKUP, RESTORE } operation;
static bool upload_open, upload_verified, upload_entered;
static void response(ph_http_exchange *x,unsigned status,const char *type,const void *body,size_t n)
{ x->response.status=status;x->response.type=type;x->response.body=body;x->response.length=n; }
static void tc1_memory_extra(ph_writer *w)
{
    extern int get_last_reset_reason(void);
    static bool sampled;static int reason;
    if(!sampled){reason=get_last_reset_reason();sampled=true;}
    phw_int(w,"last_reset_reason",reason);
}
#define PH_HTTP_MEMORY_EXTRA tc1_memory_extra
#include "../../common/httpd/memory_mp.inc"

/* Preserve the registered TC1 anchored routes' query/trailing-slash aliases.
 * Assets remain a deliberate prefix route with their own exact file check. */
static bool route_matches(const char *path,const char *route)
{
    size_t n=strlen(route);
    if(strncmp(path,route,n))return false;
    path+=n;
    if(*path=='?')return true;
    while(*path=='/')++path;
    return !*path;
}
static ph_http_action prepare(ph_http_exchange *x)
{
    if(operation==UPLOAD) {
        if(!upload_entered) {
            upload_entered=true;
            bool rescue=route_matches(x->request->path,"/api/v1/ota/upload-unsigned");
            if(!ota_job_enter() || !tc1_ota_begin(x->request->length,rescue)) {
                ph_http_message(&product,x,400,"OTA rejected");return PH_HTTP_REPLY;
            }
        }
        x->body_idle_ms=15000;x->body_total_ms=300000;
        return PH_HTTP_READ_BODY;
    }
    ph_http_action a=ph_http_claim(&product,x,operation==BACKUP?device_backup_size():operation==QUERY?0:x->request->length,operation==COMMAND || operation==BACKUP || operation==RESTORE);
    if(a==PH_HTTP_REPLY && product.claim && operation==QUERY)
        ph_http_query(&product,x,query_action);
    if(a==PH_HTTP_READ_BODY && operation==BACKUP){
        if(device_backup_export(product.body,device_backup_size()))response(x,200,"application/octet-stream",product.body,device_backup_size());
        else ph_http_message(&product,x,409,"backup_unavailable");
        return PH_HTTP_REPLY;
    }
    return a;
}
static ph_http_action begin(ph_http_exchange *x)
{
    const ph_http_request *r=x->request;
    ph_http_product_init(&product,x);operation=DIRECT;
    upload_open=upload_verified=upload_entered=false;
    query_action=route_matches(r->path,"/api/v1/info")?"info.get":route_matches(r->path,"/api/v1/status")?"status.get":
      route_matches(r->path,"/api/v1/capabilities")?"capabilities.get":route_matches(r->path,"/api/v1/config")?"config.get":
      route_matches(r->path,"/api/v1/schedules")?"schedules.get":route_matches(r->path,"/api/v1/logs")?"logs.get":NULL;
    bool asset=route_matches(r->path,"/") || !strncmp(r->path,"/assets",7);
    bool command=route_matches(r->path,"/api/v1/command");
    bool backup=route_matches(r->path,"/api/v1/config/backup"),restore=route_matches(r->path,"/api/v1/config/restore");
    bool ota=route_matches(r->path,"/api/v1/ota/upload") || route_matches(r->path,"/api/v1/ota/upload-unsigned");
    bool memory=route_matches(r->path,"/api/v1/memory"),trace=route_matches(r->path,"/api/v1/http-trace");
    /* TC1 historically selects an actual route before OPTIONS/authentication. */
    if(!query_action && !asset && !command && !backup && !restore && !ota && !memory && !trace) {
        response(x,404,"text/plain","",0);return PH_HTTP_REPLY;
    }
    x->response.cors=phicomm_cors_headers(r->origin);
    if(!strcmp(r->method,"OPTIONS")) {
        response(x,*x->response.cors?200:403,"text/plain","",0);return PH_HTTP_REPLY;
    }
    if(!tc1_http_authorized(r->authorization)) {
        response(x,401,"text/plain","",0);x->response.realm="TC1";return PH_HTTP_REPLY;
    }
    if(backup || restore){
        if(!user_config->web_password[0]){ph_http_message(&product,x,403,"set_web_password_first");return PH_HTTP_REPLY;}
        if(backup && !strcmp(r->method,"GET")){operation=BACKUP;return prepare(x);}
        if(restore && !strcmp(r->method,"POST") && r->have_length && r->length==device_backup_size()){operation=RESTORE;return prepare(x);}
        ph_http_message(&product,x,400,"invalid_backup");return PH_HTTP_REPLY;
    }
    if(!strcmp(r->method,"GET") || !strcmp(r->method,"HEAD")) {
        x->response.suppress_body=!strcmp(r->method,"HEAD");
        if(query_action) { operation=QUERY;return prepare(x); }
        if(memory) { memory_response(x);return PH_HTTP_REPLY; }
        if(trace) {
            ph_writer w;if(!phw_init(&w,PH_WIRE_MSGPACK)){ph_http_busy(x);return PH_HTTP_REPLY;}
            tc1_http_trace_mp(&w);ph_http_wire_take(&product,x,phw_take(&w));return PH_HTTP_REPLY;
        }
        if(asset && (route_matches(r->path,"/") || !strcmp(r->path,"/assets/index.html"))) {
            response(x,200,"text/html",web_index_html,sizeof(web_index_html));x->response.gzip=true;
            return PH_HTTP_REPLY;
        }
    }
    if(!strcmp(r->method,"POST") && command) {
        if(!r->length || r->length>(ota_job_active()?1024u:8192u)) {
            response(x,400,"text/plain","",0);return PH_HTTP_REPLY;
        }
        operation=COMMAND;return prepare(x);
    }
    if(!strcmp(r->method,"POST") && ota) {
        if(!r->length || !ota_job_claim()) { ph_http_busy(x);return PH_HTTP_REPLY; }
        if(!tc1_ota_reserve()) { ota_job_release();ph_http_busy(x);return PH_HTTP_REPLY; }
        upload_open=true;operation=UPLOAD;
        /* Return to core once so verified admission=2 is installed before the
         * synchronous flash/signature initialization callback runs. */
        return PH_HTTP_WAIT;
    }
    response(x,404,"text/plain","",0);return PH_HTTP_REPLY;
}
static bool body_chunk(ph_http_exchange *x,const void *data,size_t n)
{
    if(operation==UPLOAD) {
        if(tc1_ota_write(data,n))return true;
        ph_http_message(&product,x,400,"OTA rejected");return false;
    }
    return ph_http_body_copy(&product,data,n);
}
static void complete(ph_http_exchange *x)
{
    if(operation==UPLOAD) {
        upload_verified=tc1_ota_verify();
        ph_http_message(&product,x,upload_verified?202:400,upload_verified?"文件校验通过，设备正在重启":"OTA rejected");return;
    }
    if(operation==RESTORE){const char *code=device_backup_import(product.body,product.used);ph_http_message(&product,x,!strcmp(code,"accepted")?202:!strcmp(code,"busy")?409:400,code);return;}
    ph_http_command_mp(&product,x,product.body,product.used);
    /* Request is borrowed synchronously; response owns all output bytes. */
}
static void release(ph_http_exchange *x,bool sent)
{
    (void)x;
    ph_http_product_release(&product);
    if(upload_open && !upload_verified) { tc1_ota_abort();ota_job_release(); }
    if(upload_verified) {
        if(!sent) {
            /* Verification alone changed no boot metadata. A lost response
             * neither authorizes reset nor makes the passive slot permanent. */
            tc1_ota_failure("HTTP response not delivered; upload not activated");
            ota_job_release();
        } else if(tc1_ota_activate()) {
            mico_thread_msleep(300);
            mico_system_power_perform(mico_system_context_get(),eState_Software_Reset);
        } else if(!tc1_ota_commit_uncertain()) {
            tc1_ota_abort();ota_job_release();
        }
        /* An uncertain activation retains both reservations, observable as
         * failed, with no automatic retry or reset. */
    }
    upload_open=false;
}
static unsigned admission(void) { return ota_job_active()?2:PH_HTTP_CLIENT_SLOTS; }
static const ph_http_app app={begin,prepare,body_chunk,complete,release,admission};
int AppHttpdStart(void) { return ph_http_start(&app,PH_HTTP_CLIENT_SLOTS); }
int AppHttpdStop(void) { return ph_http_stop(); }
