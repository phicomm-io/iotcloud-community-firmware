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
#include "../../common/http_cors.h"
#include "../../common/firmware_variant.h"
#include "../../common/ota/mode.h"
#include "json_buffer.h"
#include <web_data.c>
static ph_http_product product;
static const char *query_action;
static enum { DIRECT, QUERY, COMMAND, UPLOAD } operation;
static bool upload_open, upload_verified, upload_entered;
static void response(ph_http_exchange *x,unsigned status,const char *type,const void *body,size_t n)
{ x->response.status=status;x->response.type=type;x->response.body=body;x->response.length=n; }
json_object *tc1_status_json(void) {
    unsigned long free_memory = (unsigned long)MicoGetMemoryInfo()->free_memory;
    /* Keep the v1 command status free of obsolete duplicate fields. */
    size_t capacity = 2048;
    capacity += (jb_string_size(sys_config->micoSystemConfig.name) +
                     jb_string_size(sys_config->micoSystemConfig.ssid) +
                     jb_string_size(user_config->ap_name) + jb_string_size(MQTT_SERVER) +
                     jb_string_size(MQTT_SERVER_USR));
    for (int i = 0; i < SOCKET_NUM; ++i)
        capacity += jb_string_size(user_config->socket_names[i]);
    char *data = malloc(capacity);
    if (!data)
        return NULL;
    JsonBuffer b = {data, 0, capacity, true};
    mico_time_t now;
    mico_time_get_time(&now);
    jb_printf(&b, "{");
    jb_printf(&b, "\"utc_time\":%ld,\"time_synced\":%s,", (long)time(NULL),
              rtc_init == 1 ? "true" : "false");
    jb_field(&b, "wifi_trial_status", WifiTrialStatus());
    jb_field(&b, "version", VERSION);
    jb_field(&b, "device_id", str_mac);
    jb_field(&b, "name", sys_config->micoSystemConfig.name);
    jb_field(&b, "ssid", sys_config->micoSystemConfig.ssid);
    jb_field(&b, "ip", ip_status.ip);
    jb_field(&b, "mask", ip_status.mask);
    jb_field(&b, "gateway", ip_status.gateway);
    jb_field(&b, "ap_name", user_config->ap_name);
    jb_field(&b, "mqtt_host", MQTT_SERVER);
    jb_field(&b, "mqtt_user", MQTT_SERVER_USR);
    jb_field(&b, "mqtt_topic_mode", user_config->mqtt_topic_mode ? "tmcp" : "legacy");
    jb_field(&b, "mqtt_protocol", user_config->mqtt_topic_mode ? "tmcp" : "legacy");
    jb_printf(&b, "\"mqtt_protocol_version\":%d,", user_config->mqtt_topic_mode ? 2 : 1);
    jb_field(&b, "mqtt_status", mqtt_status_name(tc1_mqtt_status));
    jb_printf(&b, "\"socket_names\":[");
    for (int i = 0; i < SOCKET_NUM; ++i) {
        if (i)
            jb_printf(&b, ",");
        jb_string(&b, user_config->socket_names[i]);
    }
    jb_printf(&b, "],");
    jb_printf(&b, "\"socket_states\":[%s],", GetSocketStatus());
    jb_printf(&b, "\"mqtt_port\":%d,\"led_enabled\":%s,\"child_lock\":%s,",
              MQTT_SERVER_PORT, user_config->power_led_enabled ? "true" : "false",
              childLockEnabled ? "true" : "false");
    jb_printf(&b, "\"report_seconds\":%d,\"powerCoefficientX100\":%u,\"uptime_seconds\":%lu,\"free_memory\":%lu,",
              MQTT_REPORT_FREQ, user_config->power_coefficient_x100,
              (unsigned long)now / 1000, free_memory);
    jb_printf(&b,
              "\"wifi_connected\":%s,\"ap_active\":%s,\"wifi_password_set\":%s,\"mqtt_password_"
              "set\":%s,\"web_password_set\":%s,\"mqtt_cleanup_warning\":%s,\"mqtt_connected\":%s,",
              wifi_status == WIFI_STATE_CONNECTED ? "true" : "false",
              ip_status.mode == 0 ? "true" : "false",
              sys_config->micoSystemConfig.user_key[0] ? "true" : "false",
              user_config->mqtt_password[0] ? "true" : "false",
              user_config->web_password[0] ? "true" : "false",
              tc1_mqtt_cleanup_warning ? "true" : "false",
              tc1_mqtt_status == MQTT_READY ? "true" : "false");
    char state[32], error[128];
    unsigned progress;
    tc1_ota_snapshot(state, error, &progress);
    jb_field(&b, "ota_state", state);
    jb_field(&b, "ota_error", error);
    jb_printf(&b, "\"ota_progress\":%u}", progress);
    json_object *out = b.valid ? service_parse(b.data,b.used) : NULL;
    free(data);
    if(out){phicomm_variant_json(out);json_build_object_add(out,"model",json_object_new_string("tc1"));}
    return out;
}

/* Build only fields needed by high-frequency UI reads: no full buffer/parse. */
json_object *tc1_status_live_json(void) {
    json_object *o=json_object_new_object(),*states=json_object_new_array();
    if(!o){if(states)json_object_put(states);return NULL;}
    json_build_object_add(o,"model",json_object_new_string("tc1"));
    json_build_object_add(o,"version",json_object_new_string(VERSION));
    json_build_object_add(o,"device_id",json_object_new_string(str_mac));
    json_build_object_add(o,"name",json_object_new_string(sys_config->micoSystemConfig.name));
    mico_time_t now;mico_time_get_time(&now);
    json_build_object_add(o,"uptime_seconds",json_object_new_int((unsigned long)now/1000));
    for(unsigned i=0;i<SOCKET_NUM;i++)json_build_array_add(states,json_object_new_int(user_config->socket_status[i]));
    json_build_object_add(o,"socket_states",states);
    json_build_object_add(o,"child_lock",json_object_new_boolean(childLockEnabled));
    json_build_object_add(o,"led_enabled",json_object_new_boolean(user_config->power_led_enabled));
    json_build_object_add(o,"wifi_connected",json_object_new_boolean(wifi_status==WIFI_STATE_CONNECTED));
    json_build_object_add(o,"mqtt_connected",json_object_new_boolean(tc1_mqtt_status==MQTT_READY));
    json_build_object_add(o,"ap_active",json_object_new_boolean(ip_status.mode==0));
    json_build_object_add(o,"mqtt_cleanup_warning",json_object_new_boolean(tc1_mqtt_cleanup_warning));
    json_build_object_add(o,"mqtt_status",json_object_new_string(mqtt_status_name(tc1_mqtt_status)));
    json_build_object_add(o,"wifi_trial_status",json_object_new_string(WifiTrialStatus()));
    char state[32],error[128];unsigned progress;tc1_ota_snapshot(state,error,&progress);
    json_build_object_add(o,"ota_state",json_object_new_string(state));
    json_build_object_add(o,"ota_error",json_object_new_string(error));
    json_build_object_add(o,"ota_progress",json_object_new_int(progress));
    return o;
}

static void memory_response(ph_http_exchange *x){
    extern int get_last_reset_reason(void);
    static bool reset_sampled;static int reset_reason;
    if(!reset_sampled){reset_reason=get_last_reset_reason();reset_sampled=true;}
    const micoMemInfo_t *m=MicoGetMemoryInfo();if(!m)return;
    micoMemInfo_t snapshot=*m;service_memory_state_t b;service_memory_snapshot(&b);
    ph_http_diagnostics d=ph_http_snapshot();
    int tcp_limit=d.limit,tcp_connections=d.connections,tcp_rc=d.connections_get_rc;
    char *out=product.scalar;int n=snprintf(out,sizeof(product.scalar),
        "{\"code\":\"ok\",\"data\":{\"free_memory\":%d,\"allocated_memory\":%d,"
        "\"free_memory_chunks\":%d,\"uptime_ms\":%lu,\"last_reset_reason\":%d,\"memory_reserve\":%u,"
        "\"http_tcp_limit\":%d,\"http_tcp_connections\":%d,\"http_tcp_get_rc\":%d,"
        "\"memory_reserved\":%lu,\"memory_active\":%lu,\"memory_waiting\":%lu,"
        "\"memory_denied\":%lu,\"minimum_sampled_free\":%lu,"
        "\"json_live_blocks\":%lu,\"json_peak_blocks\":%lu,\"json_alloc_failures\":%lu,\"json_guard_denied\":%lu,"
        "\"mqtt_pending\":%lu,\"mqtt_peak\":%lu,\"mqtt_overflow\":%lu,\"mqtt_max_delay_ms\":%lu}}",
        snapshot.free_memory,snapshot.allocted_memory,snapshot.num_of_chunks,
        (unsigned long)mico_rtos_get_time(),reset_reason,SERVICE_MEMORY_RESERVE,tcp_limit,tcp_connections,tcp_rc,
        (unsigned long)b.reserved,(unsigned long)b.active,(unsigned long)b.waiting,
        (unsigned long)b.denied,(unsigned long)b.minimum_free,
        (unsigned long)b.json_live,(unsigned long)b.json_peak,
        (unsigned long)b.json_failed,(unsigned long)b.json_denied,
        (unsigned long)b.mqtt_pending,(unsigned long)b.mqtt_peak,
        (unsigned long)b.mqtt_overflow,(unsigned long)b.mqtt_max_delay);
    if(n>0)n=ph_http_diag_append(out,sizeof(product.scalar),n,&d);
    if(n>0 && (size_t)n<sizeof(product.scalar))response(x,200,"application/json",out,n);
}

static ph_http_action prepare(ph_http_exchange *x)
{
    if(operation==UPLOAD) {
        if(!upload_entered) {
            upload_entered=true;
            bool rescue=!strcmp(x->request->path,"/api/v1/ota/upload-unsigned");
            if(!ota_job_enter() || !tc1_ota_begin(x->request->length,rescue)) {
                ph_http_message(&product,x,400,"OTA rejected");return PH_HTTP_REPLY;
            }
        }
        x->body_idle_ms=15000;x->body_total_ms=300000;
        return PH_HTTP_READ_BODY;
    }
    ph_http_action a=ph_http_claim(&product,x,operation==QUERY?0:x->request->length,operation==COMMAND);
    if(a==PH_HTTP_REPLY && product.claim && operation==QUERY)
        ph_http_json(&product,x,service_query(query_action));
    return a;
}
static ph_http_action begin(ph_http_exchange *x)
{
    const ph_http_request *r=x->request;
    ph_http_product_init(&product,x);operation=DIRECT;
    upload_open=upload_verified=upload_entered=false;
    query_action=!strcmp(r->path,"/api/v1/info")?"info.get":!strcmp(r->path,"/api/v1/status")?"status.get":
      !strcmp(r->path,"/api/v1/capabilities")?"capabilities.get":!strcmp(r->path,"/api/v1/config")?"config.get":
      !strcmp(r->path,"/api/v1/schedules")?"schedules.get":!strcmp(r->path,"/api/v1/logs")?"logs.get":NULL;
    bool asset=!strcmp(r->path,"/") || !strncmp(r->path,"/assets",7);
    bool command=!strcmp(r->path,"/api/v1/command");
    bool ota=!strcmp(r->path,"/api/v1/ota/upload") || !strcmp(r->path,"/api/v1/ota/upload-unsigned");
    bool memory=!strcmp(r->path,"/api/v1/memory"),trace=!strcmp(r->path,"/api/v1/http-trace");
    /* TC1 historically selects an actual route before OPTIONS/authentication. */
    if(!query_action && !asset && !command && !ota && !memory && !trace) {
        response(x,404,"text/plain","",0);return PH_HTTP_REPLY;
    }
    x->response.cors=phicomm_cors_headers(r->origin);
    if(!strcmp(r->method,"OPTIONS")) {
        response(x,*x->response.cors?200:403,"text/plain","",0);return PH_HTTP_REPLY;
    }
    if(!tc1_http_authorized(r->authorization)) {
        response(x,401,"text/plain","",0);x->response.realm="TC1";return PH_HTTP_REPLY;
    }
    if(!strcmp(r->method,"GET") || !strcmp(r->method,"HEAD")) {
        x->response.suppress_body=!strcmp(r->method,"HEAD");
        if(query_action) { operation=QUERY;return prepare(x); }
        if(memory) { memory_response(x);return PH_HTTP_REPLY; }
        if(trace) {
            size_t n=tc1_http_trace_format(product.scalar,sizeof(product.scalar));
            if(n)response(x,200,"application/json",product.scalar,n);return PH_HTTP_REPLY;
        }
        if(asset && (!strcmp(r->path,"/") || !strcmp(r->path,"/assets/index.html"))) {
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
        upload_verified=tc1_ota_finish();
        ph_http_message(&product,x,upload_verified?202:400,upload_verified?"文件校验通过，设备正在重启":"OTA rejected");return;
    }
    json_object *request=ph_http_body_json(&product,x);
    ph_http_json(&product,x,service_memory_failures()==product.failures?service_command(request):NULL);
    if(request)json_object_put(request);
}
static void release(ph_http_exchange *x,bool sent)
{
    (void)x;
    ph_http_product_release(&product);
    if(upload_open && !upload_verified) { tc1_ota_abort();ota_job_release(); }
    if(upload_verified && sent) {
        /* Preserve commit reservation until reset and the existing post-response
         * transmission grace. This is an explicit successful OTA action. */
        mico_thread_msleep(300);
        mico_system_power_perform(mico_system_context_get(),eState_Software_Reset);
    }
    upload_open=false;
}
static unsigned admission(void) { return ota_job_active()?2:3; }
static const ph_http_app app={begin,prepare,body_chunk,complete,release,admission};
int AppHttpdStart(void) { return ph_http_start(&app,3,false); }
int AppHttpdStop(void) { return ph_http_stop(); }
