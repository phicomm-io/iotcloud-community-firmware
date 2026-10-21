#include "../../common/service.h"
#include "../../common/firmware_variant.h"
#include "../../common/http_cors.h"
const char *phicomm_http_cors(const char *origin){return phicomm_cors_headers(origin);}
#include "../../common/ota/signed_ota.h"
#include "ota_server/ota_server.h"
#include "ota_server/signed_adapter.h"
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

#include <time.h>
#include <httpd.h>
#include <http_parse.h>
#include <http-strings.h>
#include "stdlib.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mico.h"
#include "httpd_priv.h"
#include "app_httpd.h"
#include "../../common/ota/mode.h"
#include "../../common/ota/fetch.h"
#include "user_gpio.h"
#include "user_wifi.h"
#include "user_power.h"
#include "main.h"
#include "http_trace.h"
#include <web_data.c>
#include "web_log.h"
#include "timed_task/timed_task.h"
#include "ota_server/user_ota.h"
#include "mqtt_server/user_mqtt_client.h"
#include "json.h"
#include "json_buffer.h"
#include "printbuf.h"
#include "json_heap.h"
#include "../../common/memory_wait.h"

/* Parse headers once, reject truncation/chunked bodies and read all fragments.
 * capacity includes the terminator. An incomplete request never mutates config. */
static int read_headers(httpd_request_t *req) {
    if (!req->hdr_parsed) {
        char *headers = malloc(HTTPD_MAX_MESSAGE);
        if (!headers)
            return kNoMemoryErr;
        int err = httpd_parse_hdr_tags(req, req->sock, headers, HTTPD_MAX_MESSAGE);
        free(headers);
        if (err != kNoErr)
            return err;
        req->hdr_parsed = 1;
    }
    return kNoErr;
}
static int read_body(httpd_request_t *req, char *out, int capacity) {
    if(ota_job_active() && strcmp(req->filename,"/api/v1/command"))return kGeneralErr;
    if (!out || capacity < 1) return kParamErr;
    out[0] = 0;
    int err = read_headers(req);
    if (err != kNoErr) return err;
    int length = req->remaining_bytes;
    if (req->chunked || length < 0 || length >= capacity)
        return kParamErr;
    int used = 0;
    while (used < length) {
        int n = httpd_recv(req->sock, out + used, length - used, 0);
        if (n <= 0)
            return kGeneralErr;
        used += n;
        req->remaining_bytes -= n;
    }
    out[used] = 0;
    if (memchr(out, 0, used))
        return kParamErr;
    return kNoErr;
}
static int response_json(httpd_request_t *req, const char *code, const char *data, size_t size) {
    int err = httpd_send_all_header(req, code, size, "application/json");
    if (err != kNoErr)
        return err;
    return httpd_send_body(req->sock, (const unsigned char *)data, size);
}
static int result_message(httpd_request_t *req, bool valid, bool json) {
    if (json) {
        const char *text = valid ? "{\"message\":\"请求已接受\"}" : "{\"message\":\"配置无效\"}";
        return response_json(req, valid ? HTTP_RES_200 : HTTP_RES_400, text, strlen(text));
    }
    const char *text = valid ? "OK" : "NO";
    int err =
        httpd_send_all_header(req, valid ? HTTP_RES_200 : HTTP_RES_400, strlen(text), "text/plain");
    return err == kNoErr ? httpd_send_body(req->sock, (const unsigned char *)text, strlen(text))
                         : err;
}
static json_object *body_json(const char *body) {
    json_object *o = json_tokener_parse(body);
    if (o && !json_object_is_type(o, json_type_object)) {
        json_object_put(o);
        o = NULL;
    }
    return o;
}
static bool json_integer(json_object *o, const char *key, int *value, int low, int high) {
    json_object *v = json_object_object_get(o, key);
    if (!v || !json_object_is_type(v, json_type_int))
        return false;
    *value = json_object_get_int(v);
    return *value >= low && *value <= high;
}
static bool json_enabled(json_object *o, int *value) {
    json_object *v = json_object_object_get(o, "enabled");
    if (!v || !json_object_is_type(v, json_type_boolean))
        return false;
    *value = json_object_get_boolean(v);
    return true;
}
static bool mqtt_json_string(json_object *o, const char *key, char *out, size_t capacity,
                             bool keep_blank);
static bool is_http_init;
static bool is_handlers_registered;
const struct httpd_wsgi_call g_app_handlers[];

char up_time[16] = "00:00:00";
#define CHUNK_SIZE 512 // 姣忔鍙戦€?512 瀛楄妭锛岄伩鍏?buffer 澶ぇ
#define OTA_BUFFER_SIZE 512
#define MAX_OTA_SIZE 1024 * 1024

/*
void GetPraFromUrl(char* url, char* pra, char* val)
{
    char* sub = strstr(url, pra);
    if (sub == NULL)
    {
        val[0] = 0;
        return;
    }
    sub = strstr(sub, "=");
    if (sub == NULL)
    {
        val[0] = 0;
        return;
    }
    int len = strlen(sub);
    int n = 0;
    for (int i = 0; i < len; i++)
    {
        if (sub[i] == '&' || i == len - 1)
        {
            n = len;
            break;
        }
    }
    if (n > 0)
    {
        strncpy(val, sub + 1, n - 1);
        val[n - 1] = 0;
        return;
    }
    val[0] = 0;
}
*/

static OSStatus send_in_chunks(int sock, const uint8_t *data, int total_len) {
    OSStatus err = kNoErr;
    for (int offset = 0; offset < total_len; offset += CHUNK_SIZE) {
        int chunk_len = (total_len - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_len - offset);
        err = httpd_send_body(sock, data + offset, chunk_len);
        require_noerr_action(err, exit, http_log("ERROR: Send chunk failed at offset %d", offset));
    }
exit:
    return err;
}

static int HttpGetIndexPage(httpd_request_t *req) {
    OSStatus err = kNoErr;
    int total_sz = sizeof(web_index_html);

    err = httpd_send_all_header(req, HTTP_RES_200, total_sz, HTTP_CONTENT_HTML_ZIP);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send index headers."));

    err = send_in_chunks(req->sock, web_index_html, total_sz);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send index body."));

exit:
    return err;
}

static int HttpGetAssets(httpd_request_t *req) {
    if (!strcmp(req->filename, "/assets/index.html"))
        return HttpGetIndexPage(req);
    return httpd_send_all_header(req, HTTP_RES_404, 0, "text/plain");
}

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

static int HttpGetTc1Status(httpd_request_t *req) {
    json_object *o=tc1_status_json();if(!o)return kNoMemoryErr;
    const char *s=json_object_to_json_string(o);int rc=response_json(req,HTTP_RES_200,s,strlen(s));json_object_put(o);return rc;
}
/* Bound SDK RX storage while allowing a completed browser socket to close. */
int phicomm_http_client_limit(void){return 2;}
static int HttpV1Busy(httpd_request_t *r){
    static const char busy[]="{\"code\":\"busy\"}";
    return response_json(r,"HTTP/1.1 503 Service Unavailable\r\n",busy,sizeof(busy)-1);
}
/* A rejected POST still has unread TCP data. Consume only bounded bytes
 * without heap storage before the SDK closes the socket, avoiding a reset
 * which can hide an otherwise complete busy response from the peer. */
static int HttpV1CommandBusy(httpd_request_t *r){
    int rc=HttpV1Busy(r);
    if(rc || r->remaining_bytes<=0 || fcntl(r->sock,F_SETFL,O_NONBLOCK)!=0)return rc;
    uint32_t deadline=mico_rtos_get_time()+100u;
    char discard[128];
    for(unsigned reads=0;reads<64 && r->remaining_bytes>0;reads++){
        uint32_t now=mico_rtos_get_time();
        if((int32_t)(now-deadline)>=0)break;
        fd_set readable;FD_ZERO(&readable);FD_SET(r->sock,&readable);
        struct timeval tv={0,(deadline-now)*1000};
        if(select(r->sock+1,&readable,NULL,NULL,&tv)<=0)break;
        size_t n=r->remaining_bytes<(int)sizeof(discard)?(size_t)r->remaining_bytes:sizeof(discard);
        int got=recv(r->sock,discard,n,0);
        if(got<=0)break;
        r->remaining_bytes-=got;
    }
    return rc;
}
static int HttpV1Reply(httpd_request_t *req,json_object *o){
    if(ota_job_active())mico_thread_msleep(10);
    struct printbuf *wire=o?json_object_to_json_string_ex(o):NULL;
    if(o)json_object_put(o);
    if(!wire)return HttpV1Busy(req);
    int rc=response_json(req,HTTP_RES_200,wire->buf,wire->bpos);
    printbuf_free(wire);return rc;
}
static int HttpV1Query(httpd_request_t *r,const char *action){
    unsigned claim=service_memory_wait(0);if(!claim)return HttpV1Busy(r);
    int rc=HttpV1Reply(r,service_query(action));service_memory_release(claim);return rc;
}
static int HttpV1Info(httpd_request_t *r){return HttpV1Query(r,"info.get");}
static int HttpV1Status(httpd_request_t *r){return HttpV1Query(r,"status.get");}
static int HttpV1Capabilities(httpd_request_t *r){return HttpV1Query(r,"capabilities.get");}
static int HttpV1Config(httpd_request_t *r){return HttpV1Query(r,"config.get");}
static int HttpV1Schedules(httpd_request_t *r){return HttpV1Query(r,"schedules.get");}
static int HttpV1Logs(httpd_request_t *r){return HttpV1Query(r,"logs.get");}
static int HttpV1Memory(httpd_request_t *r){
    extern int get_last_reset_reason(void);
    static bool reset_sampled;static int reset_reason;
    if(!reset_sampled){reset_reason=get_last_reset_reason();reset_sampled=true;}
    const micoMemInfo_t *m=MicoGetMemoryInfo();if(!m)return HttpV1Busy(r);
    micoMemInfo_t snapshot=*m;service_memory_state_t b;service_memory_snapshot(&b);
    extern int phicomm_http_tcp_snapshot(int *,int *);
    int tcp_limit,tcp_connections;int tcp_rc=phicomm_http_tcp_snapshot(&tcp_limit,&tcp_connections);
    char out[896];int n=snprintf(out,sizeof(out),
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
    return n>0 && (size_t)n<sizeof(out)?response_json(r,HTTP_RES_200,out,n):HttpV1Busy(r);
}
static int HttpV1Trace(httpd_request_t *r){
    extern size_t tc1_http_trace_format(char *,size_t);
    char body[1536];size_t size=tc1_http_trace_format(body,sizeof(body));
    return size?response_json(r,HTTP_RES_200,body,size):HttpV1Busy(r);
}
static int HttpV1Command(httpd_request_t *r){
    HTTP_TRACE(HTTP_TRACE_HEADERS_ENTER,r->sock,0);
    int rc=read_headers(r);
    HTTP_TRACE(HTTP_TRACE_HEADERS_RETURN,r->sock,rc);
    if(rc || r->chunked || r->remaining_bytes<1 || r->remaining_bytes>(ota_job_active()?1024:8192))return kParamErr;
    unsigned claim=service_memory_wait(r->remaining_bytes);if(!claim)return HttpV1CommandBusy(r);
    size_t capacity=(size_t)r->remaining_bytes+1;
    uint64_t failures=service_memory_failures();
    char *body=json_heap_malloc(capacity);if(!body){service_memory_release(claim);return HttpV1CommandBusy(r);}
    HTTP_TRACE(HTTP_TRACE_BODY_ENTER,r->sock,capacity);
    rc=read_body(r,body,capacity);
    HTTP_TRACE(HTTP_TRACE_BODY_RETURN,r->sock,rc);
    if(rc){
        /* A truncated/reset request has no reply channel. Close without
         * dispatching it or attempting another write to the failed socket. */
        json_heap_free(body);service_memory_release(claim);return HTTPD_DONE;
    }
    HTTP_TRACE(HTTP_TRACE_PARSE_ENTER,r->sock,capacity);
    json_object *request=service_parse(body,strlen(body));json_heap_free(body);
    HTTP_TRACE(HTTP_TRACE_PARSE_RETURN,r->sock,request!=NULL);
    HTTP_TRACE(HTTP_TRACE_DISPATCH_ENTER,r->sock,0);
    json_object *reply=service_memory_failures()==failures?service_command(request):NULL;if(request)json_object_put(request);
    HTTP_TRACE(HTTP_TRACE_DISPATCH_RETURN,r->sock,reply!=NULL);
    HTTP_TRACE(HTTP_TRACE_REPLY_ENTER,r->sock,0);
    rc=HttpV1Reply(r,reply);service_memory_release(claim);
    HTTP_TRACE(HTTP_TRACE_REPLY_RETURN,r->sock,rc);return rc;
}

static int HttpSetSocketStatus(httpd_request_t *req) {
    char buf[256];
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    if (buf[0] == '{') {
        json_object *o = body_json(buf), *v = o ? json_object_object_get(o, "states") : NULL;
        bool valid = v && json_object_is_type(v, json_type_array) &&
                     json_object_array_length(v) == SOCKET_NUM;
        int values[SOCKET_NUM];
        for (int i = 0; valid && i < SOCKET_NUM; ++i) {
            json_object *x = json_object_array_get_idx(v, i);
            valid = json_object_is_type(x, json_type_boolean);
            values[i] = json_object_get_boolean(x);
        }
        if (valid)
            for (int i = 0; i < SOCKET_NUM; ++i) {
                UserRelaySet(i, values[i]);
                UserMqttSendSocketState(i);
            }
        if (valid) {
            mico_system_context_update(sys_config);
            UserMqttSendTotalSocketState();
        }
        if (o)
            json_object_put(o);
        return result_message(req, valid, true);
    }
    int values[6];
    char extra;
    bool valid = sscanf(buf, "%d,%d,%d,%d,%d,%d %c", &values[0], &values[1], &values[2], &values[3],
                        &values[4], &values[5], &extra) == 6;
    for (int i = 0; valid && i < 6; ++i)
        valid = values[i] == 0 || values[i] == 1;
    if (valid)
        SetSocketStatus(buf);
    return result_message(req, valid, false);
}

static int HttpSetSocketName(httpd_request_t *req) {
    char buf[512], name[SOCKET_NAME_LENGTH] = {0};
    int index = -1;
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{', valid = false;
    json_object *o = json ? body_json(buf) : NULL;
    if (o)
        valid = json_integer(o, "index", &index, 0, SOCKET_NUM - 1) &&
                mqtt_json_string(o, "name", name, sizeof(name), false) && name[0];
    else if (!json) {
        char extra;
        valid = sscanf(buf, "%d %63s %c", &index, name, &extra) == 2 && index >= 0 &&
                index < SOCKET_NUM;
    }
    if (valid) {
        strcpy(user_config->socket_names[index], name);
        mico_system_context_update(sys_config);
        registerMqttEvents();
    }
    if (o)
        json_object_put(o);
    return result_message(req, valid, json);
}

static int HttpSetButtonEvent(httpd_request_t *req) {
    char buf[256];
    int index = 0, func = -2, longPress = -1;
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{', valid = false;
    json_object *o = json ? body_json(buf) : NULL;
    if (o)
        valid = json_integer(o, "index", &index, 1, 30) &&
                json_integer(o, "operation", &func, -1, 13) &&
                json_integer(o, "long_press", &longPress, 0, 1);
    else if (!json) {
        char extra;
        valid = sscanf(buf, "%d %d %d %c", &index, &func, &longPress, &extra) == 3 && index >= 1 &&
                index <= 30 && func >= -1 && func <= 13 && (longPress == 0 || longPress == 1);
    }
    if (valid) {
        set_key_map(user_config->user, index,
                    longPress    ? get_short_func(user_config->user[index])
                    : func == -1 ? NO_FUNCTION
                                 : func,
                    longPress ? (func == -1 ? NO_FUNCTION : func)
                              : get_long_func(user_config->user[index]));
        mico_system_context_update(sys_config);
    }
    if (o)
        json_object_put(o);
    return result_message(req, valid, json);
}
#define OTA_BUF_SIZE 5120

static int HttpUploadOTA(httpd_request_t *req, bool rescue) {
    OSStatus err = kGeneralErr;
    char buffer[512];
    /* The SDK parses Content-Length lazily on the first body read. */
    int first = httpd_get_data2(req, buffer, sizeof(buffer));
    if (first <= 0 || req->chunked || req->remaining_bytes < 0 )
        return kParamErr;
    if(!ota_job_claim())return kGeneralErr;
    if(!tc1_ota_reserve()){ota_job_release();return kGeneralErr;}
    if(!ota_job_enter()){tc1_ota_abort();ota_job_release();return kGeneralErr;}
    if (!tc1_ota_begin(req->remaining_bytes + first, rescue)) {
        tc1_ota_abort();ota_job_release();
        return kParamErr;
    }
    if (!tc1_ota_write((uint8_t *)buffer, first)) {
        tc1_ota_abort();ota_job_release();
        return kConnectionErr;
    }
    while (req->remaining_bytes > 0) {
        int n = httpd_get_data2(req, buffer, sizeof(buffer));
        if (n <= 0 || !tc1_ota_write((uint8_t *)buffer, n)) {
            tc1_ota_abort();ota_job_release();
            return kConnectionErr;
        }
    }
    if (!tc1_ota_finish()) {
        tc1_ota_abort();ota_job_release();
        return kGeneralErr;
    }

    err = response_json(req, "HTTP/1.1 202 Accepted\r\n",
                        "{\"message\":\"文件校验通过，设备正在重启\"}",
                        strlen("{\"message\":\"文件校验通过，设备正在重启\"}"));
    require_noerr(err, exit);
    /* Give the TCP stack time to transmit the accepted response before reset. */
    mico_rtos_thread_msleep(300);
    mico_system_power_perform(mico_system_context_get(), eState_Software_Reset);
exit:
    return err;
}
static int HttpSetOTAFile(httpd_request_t *req) { return HttpUploadOTA(req, false); }
static int HttpSetOTAUnsigned(httpd_request_t *req) { return HttpUploadOTA(req, true); }

static int HttpSetDeviceName(httpd_request_t *req) {
    char buf[512], name[64] = {0};
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{', valid = false;
    json_object *o = json ? body_json(buf) : NULL;
    if (o)
        valid = mqtt_json_string(o, "name", name, sizeof(name), false);
    else if (!json) {
        char extra;
        valid = sscanf(buf, "%63s %c", name, &extra) == 1;
    }
    if (valid) {
        if (name[0])
            strcpy(sys_config->micoSystemConfig.name, name);
        else
            snprintf(sys_config->micoSystemConfig.name,
                     sizeof(sys_config->micoSystemConfig.name), ZTC1_NAME,
                     str_mac + 8);
        mico_system_context_update(sys_config);
        registerMqttEvents();
    }
    if (o)
        json_object_put(o);
    return result_message(req, valid, json);
}

static int HttpSetChildLock(httpd_request_t *req) {
    char buf[128];
    int value = 0;
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{';
    json_object *o = json ? body_json(buf) : NULL;
    char extra;
    bool valid =
        o ? json_enabled(o, &value)
          : !json && sscanf(buf, "%d %c", &value, &extra) == 1 && (value == 0 || value == 1);
    if (valid) {
        user_config->user[0] = value;
        childLockEnabled = value;
        mico_system_context_update(sys_config);
        UserMqttSendChildLockState();
    }
    if (o)
        json_object_put(o);
    return result_message(req, valid, json);
}

static int HttpGetPowerInfo(httpd_request_t *req) {
    char buf[64] = {0};
    int idx = 0;
    if (req->type == HTTPD_REQ_TYPE_POST) {
        int err = read_body(req, buf, sizeof(buf));
        if (err)
            return err;
        sscanf(buf, "%d", &idx);
    }
    size_t capacity = 2048 + 6 * strlen(sys_config->micoSystemConfig.name);
    for (int i = 0; i < SOCKET_NUM; ++i)
        capacity += 6 * strlen(user_config->socket_names[i]);
    char *data = malloc(capacity);
    if (!data)
        return kNoMemoryErr;
    JsonBuffer b = {data, 0, capacity, true};
    mico_time_t now;
    mico_time_get_time(&now);
    snprintf(up_time, sizeof(up_time), "%lu - %02lu:%02lu:%02lu", (unsigned long)now / 86400000,
             (unsigned long)now / 3600000 % 24, (unsigned long)now / 60000 % 60,
             (unsigned long)now / 1000 % 60);
    jb_printf(&b, "{");
    jb_field(&b, "sockets", GetSocketStatus());
    jb_field(&b, "up_time", up_time);
    jb_field(&b, "deviceName", sys_config->micoSystemConfig.name);
    char names[SOCKET_NUM * SOCKET_NAME_LENGTH + SOCKET_NUM];
    snprintf(names, sizeof(names), "%s,%s,%s,%s,%s,%s", user_config->socket_names[0],
             user_config->socket_names[1], user_config->socket_names[2],
             user_config->socket_names[3], user_config->socket_names[4],
             user_config->socket_names[5]);
    jb_field(&b, "socketNames", names);
    jb_printf(
        &b,
        "\"idx\":%d,\"len\":%d,\"p_count\":%lu,\"powers\":[%s],\"power_watts\":%.2f,\"energy_kwh\":"
        "%.5f,\"led_enabled\":%d,\"total_switch_on\":%d,\"p_count_1_day_ago\":%d,\"p_count_2_days_"
        "ago\":%d,\"child_lock_enabled\":%d,\"powerCoefficientX100\":%u,\"btnClicks\":%s}",
        power_record.idx, PW_NUM, (unsigned long)p_count, GetPowerRecord(idx), real_time_power,
        PowerPulseCountToKwh(p_count), user_config->power_led_enabled, RelayOut() ? 1 : 0,
        user_config->p_count_1_day_ago, user_config->p_count_2_days_ago, childLockEnabled,
        user_config->power_coefficient_x100, GetButtonClickConfig());
    int err = b.valid ? response_json(req, HTTP_RES_200, data, b.used) : kNoMemoryErr;
    free(data);
    return err;
}

// 鍗曚釜鍗佸叚杩涘埗瀛楃杞暟瀛楋紙瀹夊叏锛?
static int hex_char_to_int(char c) {
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
    if ('A' <= c && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// 鍋ュ．鐗?URL 瑙ｇ爜鍑芥暟
void url_decode(const char *src, char *dest, size_t max_len) {
    size_t i = 0;
    while (*src && i < max_len - 1) {
        if (*src == '%') {
            if (src[1] && src[2] && isxdigit((unsigned char)src[1]) &&
                isxdigit((unsigned char)src[2])) {
                int high = hex_char_to_int(src[1]);
                int low = hex_char_to_int(src[2]);
                if (high >= 0 && low >= 0) {
                    dest[i++] = (char)((high << 4) | low);
                    src += 3;
                    continue;
                }
            }
            // 闈炴硶缂栫爜锛岃烦杩?%
            src++;
        } else if (*src == '+') {
            dest[i++] = ' ';
            src++;
        } else {
            dest[i++] = *src++;
        }
    }
    dest[i] = '\0';
}

static int HttpSetWifiConfig(httpd_request_t *req) {
    char buf[768], ssid[33] = {0}, key[65] = {0};
    int mode = 1;
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{', valid = false;
    json_object *o = json ? body_json(buf) : NULL;
    if (o) {
        json_object *v = json_object_object_get(o, "ssid");
        valid = v && mqtt_json_string(o, "ssid", ssid, sizeof(ssid), false) && ssid[0];
        if (!strcmp(ssid, sys_config->micoSystemConfig.ssid))
            snprintf(key, sizeof(key), "%s", sys_config->micoSystemConfig.user_key);
        valid = valid && mqtt_json_string(o, "password", key, sizeof(key), true);
        v = json_object_object_get(o, "open_network");
        if (v && json_object_is_type(v, json_type_boolean) && json_object_get_boolean(v))
            key[0] = 0;
        if (v && !json_object_is_type(v, json_type_boolean))
            valid = false;
    } else if (!json) {
        char ssid_enc[128], key_enc[192], extra;
        valid = sscanf(buf, "%d %127s %191s %c", &mode, ssid_enc, key_enc, &extra) == 3 &&
                (mode == 0 || mode == 1);
        if (valid) {
            url_decode(ssid_enc, ssid, sizeof(ssid));
            url_decode(key_enc, key, sizeof(key));
        }
    }
    if (valid) {
        if (mode == 1)
            valid = WifiConnect(ssid, key);
        else
            ApConfig(ssid, key);
    }
    if (o)
        json_object_put(o);
    return result_message(req, valid, json);
}

static int HttpGetWifiScan(httpd_request_t *req) {
    char *data = WifiScanJson();
    if (!data)
        return kNoMemoryErr;
    int err = response_json(req, HTTP_RES_200, data, strlen(data));
    free(data);
    return err;
}

static int HttpSetWifiScan(httpd_request_t *req) {
    char buf[16];
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    return result_message(req, WifiScanStart() == kNoErr, buf[0] == '{');
}

static int HttpSetRebootSystem(httpd_request_t *req) {
    char buf[32];
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{';
    json_object *o = json ? body_json(buf) : NULL;
    if (json && !o)
        return result_message(req, false, true);
    if (o)
        json_object_put(o);
    err = result_message(req, true, json);
    if (err)
        return err;
    mico_rtos_thread_msleep(300);
    if (!strcmp(buf, "soft"))
        UserSoftReboot();
    else
        MicoSystemReboot();
    return err;
}
static bool mqtt_json_string(json_object *o, const char *key, char *out, size_t capacity,
                             bool keep_blank) {
    json_object *v = json_object_object_get(o, key);
    if (!v)
        return true;
    if (!json_object_is_type(v, json_type_string))
        return false;
    const char *text = json_object_get_string(v);
    if (strlen(text) != (size_t)json_object_get_string_len(v) || strlen(text) >= capacity ||
        strpbrk(text, "\r\n"))
        return false;
    if (*text || !keep_blank)
        strcpy(out, text);
    return true;
}

static int HttpSetMqttConfig(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[2048] = {0}, host[32], user[64], password[257];
    strcpy(host, user_config->mqtt_host);
    strcpy(user, user_config->mqtt_user);
    strcpy(password, user_config->mqtt_password);
    int port = user_config->mqtt_port, mode = user_config->mqtt_topic_mode;
    bool valid = false;
    json_object *o = NULL;
    err = read_body(req, buf, sizeof(buf) - 1);
    require_noerr(err, exit);
    if (req->body_nbytes >= sizeof(buf) || memchr(buf, 0, req->body_nbytes))
        goto invalid;
    if (buf[0] == '{') {
        o = json_tokener_parse(buf);
        if (!o || !json_object_is_type(o, json_type_object))
            goto invalid;
        valid = mqtt_json_string(o, "host", host, sizeof(host), false) &&
                mqtt_json_string(o, "user", user, sizeof(user), false) &&
                mqtt_json_string(o, "password", password, sizeof(password), true);
        json_object *v = json_object_object_get(o, "port");
        if (v) {
            if (!json_object_is_type(v, json_type_int))
                valid = false;
            else
                port = json_object_get_int(v);
        }
        v = json_object_object_get(o, "topic_mode");
        if (v) {
            if (!json_object_is_type(v, json_type_string) ||
                json_object_get_string_len(v) != (int)strlen(json_object_get_string(v)) ||
                (strcmp(json_object_get_string(v), "legacy") &&
                 strcmp(json_object_get_string(v), "tmcp") &&
                 strcmp(json_object_get_string(v), "account")))
                valid = false;
            else
                mode = strcmp(json_object_get_string(v), "legacy") != 0;
        }
        v = json_object_object_get(o, "clear_password");
        if (v && json_object_is_type(v, json_type_boolean) && json_object_get_boolean(v))
            password[0] = 0;
    } else {
        /* Compatibility for old offline pages: bounded tokens; reject excess. */
        char extra;
        valid = sscanf(buf, "%31s %d %63s %256s %c", host, &port, user, password, &extra) == 4;
    }
    if (!valid || port < 1 || port > 65535)
        goto invalid;
    mico_rtos_suspend_all_thread();
    strcpy(user_config->mqtt_host, host);
    strcpy(user_config->mqtt_user, user);
    strcpy(user_config->mqtt_password, password);
    user_config->mqtt_port = port;
    user_config->mqtt_topic_mode = mode;
    tc1_mqtt_status = MQTT_DISCONNECTED;
    mico_rtos_resume_all_thread();
    err = mico_system_context_update(sys_config);
    require_noerr(err, exit);
    err = UserMqttInit();
    require_noerr(err, exit);
    err = result_message(req, true, buf[0] == '{');
    goto exit;
invalid:
    err = result_message(req, false, buf[0] == '{');
exit:
    if (o)
        json_object_put(o);
    return err;
}

static int HttpSetMqttReportFreq(httpd_request_t *req) {
    char buf[128];
    int value;
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{';
    json_object *o = json ? body_json(buf) : NULL;
    char extra;
    bool valid =
        o ? json_integer(o, "seconds", &value, 1, 3600)
          : !json && sscanf(buf, "%d %c", &value, &extra) == 1 && value >= 1 && value <= 3600;
    if (valid) {
        MQTT_REPORT_FREQ = value;
        mico_system_context_update(sys_config);
    }
    if (o)
        json_object_put(o);
    return result_message(req, valid, json);
}

static int HttpGetMqttReportFreq(httpd_request_t *req) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", MQTT_REPORT_FREQ);
    int err = httpd_send_all_header(req, HTTP_RES_200, strlen(buf), "text/plain");
    return err ? err : httpd_send_body(req->sock, (const unsigned char *)buf, strlen(buf));
}

static int HttpGetLog(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char *logs = GetLogRecord();
    send_http(logs, strlen(logs), exit, &err);

exit:
    return err;
}

static int HttpGetTasks(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char *tasks_str = GetTaskStr();
    if (!tasks_str)
        return kNoMemoryErr;
    send_http(tasks_str, strlen(tasks_str), exit, &err);

exit:
    if (tasks_str)
        free(tasks_str);
    return err;
}

static int HttpGetButtonEvents(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char *clicks = GetButtonClickConfig();
    send_http(clicks, strlen(clicks), exit, &err);

exit:
    return err;
}

static int HttpAddTask(httpd_request_t *req) {
    char buf[256];
    long timestamp = 0;
    int operation = 0, on = 0, weekday = 0;
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{', valid = false;
    json_object *o = json ? body_json(buf) : NULL;
    if (o) {
        int t = 0;
        valid = json_integer(o, "timestamp", &t, 1, 2147483647) &&
                json_integer(o, "operation", &operation, 0, 13) &&
                json_integer(o, "on", &on, 0, 1) && json_integer(o, "weekday", &weekday, 0, 8);
        timestamp = t;
    } else if (!json) {
        char extra;
        valid =
            sscanf(buf, "%ld %d %d %d %c", &timestamp, &operation, &on, &weekday, &extra) == 4 &&
            timestamp > 0 && timestamp <= 2147483647L;
    }
    if (valid)
        valid = AddTaskValue(timestamp, operation, on, weekday);
    if (o)
        json_object_put(o);
    return result_message(req, valid, json);
}

static int HttpDelTask(httpd_request_t *req) {
    const char *value = strstr(req->filename, "/task/");
    int timestamp;
    char extra;
    bool valid = value && sscanf(value + 6, "%d %c", &timestamp, &extra) == 1 && timestamp > 0 &&
                 DelTask(timestamp);
    return result_message(req, valid, false);
}

static int LedStatus(httpd_request_t *req) {
    char buf[2] = {MQTT_LED_ENABLED ? '1' : '0', 0};
    int err = httpd_send_all_header(req, HTTP_RES_200, 1, "text/plain");
    return err ? err : httpd_send_body(req->sock, (const unsigned char *)buf, 1);
}

static int LedSetEnabled(httpd_request_t *req) {
    char buf[128];
    int value = 0;
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{';
    json_object *o = json ? body_json(buf) : NULL;
    char extra;
    bool valid =
        o ? json_enabled(o, &value)
          : !json && sscanf(buf, "%d %c", &value, &extra) == 1 && (value == 0 || value == 1);
    if (valid) {
        MQTT_LED_ENABLED = value;
        UserLedSet(RelayOut() && value);
        UserMqttSendLedState();
        mico_system_context_update(sys_config);
    }
    if (o)
        json_object_put(o);
    return result_message(req, valid, json);
}

static int TotalSocketSetEnabled(httpd_request_t *req) {
    char buf[128];
    int value = 0;
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{';
    json_object *o = json ? body_json(buf) : NULL;
    char extra;
    bool valid =
        o ? json_enabled(o, &value)
          : !json && sscanf(buf, "%d %c", &value, &extra) == 1 && (value == 0 || value == 1);
    if (valid) {
        UserRelaySetAll(value);
        for (int i = 0; i < SOCKET_NUM; ++i)
            UserMqttSendSocketState(i);
        UserMqttSendTotalSocketState();
        mico_system_context_update(sys_config);
    }
    if (o)
        json_object_put(o);
    return result_message(req, valid, json);
}

static int Otastatus(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[16] = {0};
    sprintf(buf, "%.2f", ota_progress);
    send_http(buf, strlen(buf), exit, &err);
exit:
    return err;
}

static int OtaDetailedStatus(httpd_request_t *req) {
    OSStatus err = kNoMemoryErr;
    char state[32], error[128], counter[21];
    unsigned progress;
    tc1_ota_metadata_t meta;
    tc1_ota_metadata(&meta);
    tc1_ota_snapshot(state, error, &progress);
    json_object *o = json_object_new_object();
    if (!o)
        return err;
    json_object_object_add(o, "state", json_object_new_string(state));
    json_object_object_add(o, "error", json_object_new_string(error));
    json_object_object_add(o, "progress", json_object_new_int(progress));
    json_object_object_add(o, "signature_verified",
                           json_object_new_boolean(meta.signature_verified));
    json_object_object_add(o, "image_verified", json_object_new_boolean(meta.image_verified));
    json_object_object_add(o, "key_id", json_object_new_int(meta.key_id));
    json_object_object_add(o, "image_length", json_object_new_int(meta.image_length));
    json_object_object_add(o, "version", json_object_new_string(meta.version));
    ota_counter_decimal(meta.release_counter, counter);
    json_object_object_add(o, "release_counter", json_object_new_string(counter));
    const char *text = json_object_to_json_string(o);
    send_http(text, strlen(text), exit, &err);
exit:
    json_object_put(o);
    return err;
}
static int OtaStart(httpd_request_t *req) {
    char buf[500];
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{';
    json_object *o = json ? body_json(buf) : NULL;
    json_object *v = o ? json_object_object_get(o, "url") : NULL;
    const char *url =
        json ? (v && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : NULL)
             : buf;
    bool valid = url && ota_url_valid(url) &&
                 (!json || (size_t)json_object_get_string_len(v) == strlen(url));
    if (valid)
        err = ota_server_start((char *)url, NULL, NULL);
    if (o)
        json_object_put(o);
    if (!valid)
        return result_message(req, false, json);
    if (err != kNoErr)
        return err;
    return result_message(req, true, json);
}

static int HttpSetPowerCoefficient(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[16] = {0};
    char extra;
    int coefficient_x100;

    err = read_body(req, buf, sizeof(buf) - 1);
    require_noerr(err, exit);

    if (sscanf(buf, "%d %c", &coefficient_x100, &extra) != 1 ||
        coefficient_x100 < POWER_COEFFICIENT_MIN_X100 ||
        coefficient_x100 > POWER_COEFFICIENT_MAX_X100) {
        send_http("INVALID_COEFFICIENT", 19, exit, &err);
        goto exit;
    }

    user_config->power_coefficient_x100 = (uint16_t)coefficient_x100;
    err = mico_system_context_update(sys_config);
    require_noerr(err, exit);
    send_http("OK", 2, exit, &err);

exit:
    return err;
}

static int HttpSetWebPassword(httpd_request_t *req) {
    char buf[256], password[WEB_PASSWORD_LENGTH_MAX + 1] = {0};
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    bool json = buf[0] == '{';
    json_object *o = json ? body_json(buf) : NULL;
    bool valid = o ? json_object_object_get(o, "password") &&
                         mqtt_json_string(o, "password", password, sizeof(password), false)
                   : !json && strlen(buf) < sizeof(password);
    if (valid && !json)
        strcpy(password, buf);
    size_t n = strlen(password);
    valid = valid && (!n || n >= 8);
    for (size_t i = 0; valid && i < n; ++i)
        valid = (unsigned char)password[i] >= 32 && (unsigned char)password[i] <= 126;
    if (valid) {
        strcpy(user_config->web_password, password);
        err = mico_system_context_update(sys_config);
        if (!err)
            err = httpd_auth_init(n ? "admin" : "", password);
        valid = err == kNoErr;
    }
    if (o)
        json_object_put(o);
    memset(password, 0, sizeof(password));
    return result_message(req, valid, json);
}
/* Common schedule envelope; TC1 actions are device-specific operation codes.
 * Weekly events share days/minute/on with A1 and M1. One-shot uses timestamp. */
static int HttpScheduleGet(httpd_request_t *req) {
    struct TimedTask *tasks = malloc(MAX_TASK_NUM * sizeof(*tasks));
    if (!tasks)
        return kNoMemoryErr;
    int count = TaskSnapshot(tasks, MAX_TASK_NUM);
    int days_mask[MAX_TASK_NUM] = {0};
    for (int i = 0; i < count; ++i) {
        days_mask[i] = tasks[i].weekday == 8 ? 127
                       : tasks[i].weekday    ? 1 << (tasks[i].weekday - 1)
                                             : 0;
        for (int j = 0; days_mask[i] && j < i; ++j)
            if (days_mask[j] > 0 && tasks[j].operation == tasks[i].operation &&
                tasks[j].on == tasks[i].on &&
                tasks[j].prs_time % 86400 == tasks[i].prs_time % 86400) {
                days_mask[j] |= days_mask[i];
                days_mask[i] = -1;
                break;
            }
    }
    char *data = malloc(count * 128 + 32);
    if (!data) {
        free(tasks);
        return kNoMemoryErr;
    }
    JsonBuffer b = {data, 0, count * 128 + 32, true};
    jb_printf(&b, "{\"events\":[");
    int written = 0;
    for (int i = 0; i < count; ++i) {
        if (days_mask[i] < 0)
            continue;
        struct TimedTask *t = &tasks[i];
        int days = days_mask[i];
        jb_printf(&b, "%s{\"timestamp\":%ld,\"days\":%d,\"minute\":%ld,\"operation\":%d,\"on\":%d}",
                  written++ ? "," : "", (long)t->prs_time, days,
                  (long)(t->prs_time + 28800) % 86400 / 60, t->operation, t->on);
    }
    jb_printf(&b, "]}");
    int err = b.valid ? response_json(req, HTTP_RES_200, data, b.used) : kNoMemoryErr;
    free(data);
    free(tasks);
    return err;
}
static int HttpScheduleSet(httpd_request_t *req) {
    char buf[2048];
    int err = read_body(req, buf, sizeof(buf));
    if (err)
        return err;
    json_object *o = body_json(buf), *events = o ? json_object_object_get(o, "events") : NULL;
    bool valid = events && json_object_is_type(events, json_type_array) &&
                 json_object_array_length(events) <= 8 &&
                 (!json_object_array_length(events) || rtc_init == 1);
    struct TimedTask *tasks = calloc(56, sizeof(*tasks));
    if (!tasks) {
        if (o)
            json_object_put(o);
        return kNoMemoryErr;
    }
    int count = 0;
    time_t now = time(NULL) + 28800;
    for (int i = 0; valid && i < json_object_array_length(events); ++i) {
        json_object *e = json_object_array_get_idx(events, i);
        int days, minute, operation, on, timestamp = 0;
        valid = json_object_is_type(e, json_type_object) &&
                json_integer(e, "days", &days, 0, 127) &&
                json_integer(e, "minute", &minute, 0, 1439) &&
                json_integer(e, "operation", &operation, 0, 13) && json_integer(e, "on", &on, 0, 1);
        if (!valid)
            break;
        if (!days) {
            valid =
                json_integer(e, "timestamp", &timestamp, 1, 2147483647) && timestamp > now - 28800;
            if (!valid)
                break;
        }
        for (int day = 1; day <= 7; ++day) {
            if (days && !(days & (1 << (day - 1))))
                continue;
            struct TimedTask *t = &tasks[count++];
            t->on_use = true;
            t->operation = operation;
            t->on = on;
            if (!days) {
                t->prs_time = timestamp;
                break;
            }
            int today = (now / 86400 + 3) % 7 + 1, delta = (day - today + 7) % 7;
            if (!delta && minute * 60 <= now % 86400)
                delta = 7;
            t->prs_time = now - now % 86400 + delta * 86400 + minute * 60 - 28800;
            t->weekday = day;
        }
    }
    if (valid)
        valid = TasksReplace(tasks, count);
    free(tasks);
    if (o)
        json_object_put(o);
    return result_message(req, valid, true);
}

const struct httpd_wsgi_call g_app_handlers[] = {
    {"/api/v1/http-trace", HTTPD_HDR_DEFORT, 0, HttpV1Trace, NULL, NULL, NULL},
    {"/api/v1/memory", HTTPD_HDR_DEFORT, 0, HttpV1Memory, NULL, NULL, NULL},
    {"/api/v1/info", HTTPD_HDR_DEFORT, 0, HttpV1Info, NULL, NULL, NULL},
    {"/api/v1/status", HTTPD_HDR_DEFORT, 0, HttpV1Status, NULL, NULL, NULL},
    {"/api/v1/capabilities", HTTPD_HDR_DEFORT, 0, HttpV1Capabilities, NULL, NULL, NULL},
    {"/api/v1/config", HTTPD_HDR_DEFORT, 0, HttpV1Config, NULL, NULL, NULL},
    {"/api/v1/schedules", HTTPD_HDR_DEFORT, 0, HttpV1Schedules, NULL, NULL, NULL},
    {"/api/v1/logs", HTTPD_HDR_DEFORT, 0, HttpV1Logs, NULL, NULL, NULL},
    {"/api/v1/command", HTTPD_HDR_DEFORT, 0, NULL, HttpV1Command, NULL, NULL},
    {"/api/v1/ota/upload", HTTPD_HDR_DEFORT, 0, NULL, HttpSetOTAFile, NULL, NULL},
    {"/api/v1/ota/upload-unsigned", HTTPD_HDR_DEFORT, 0, NULL, HttpSetOTAUnsigned, NULL, NULL},
    {"/", HTTPD_HDR_DEFORT, 0, HttpGetIndexPage, NULL, NULL, NULL},
    {"/assets", HTTPD_HDR_ADD_SERVER | HTTPD_HDR_ADD_CONN_CLOSE, APP_HTTP_FLAGS_NO_EXACT_MATCH,
     HttpGetAssets, NULL, NULL, NULL},

};

static int g_app_handlers_no = sizeof(g_app_handlers) / sizeof(struct httpd_wsgi_call);

static void AppHttpRegisterHandlers() {
    int rc;
    rc = httpd_register_wsgi_handlers((struct httpd_wsgi_call *)g_app_handlers, g_app_handlers_no);
    if (rc) {
        http_log("failed to register test web handler");
    }
}

static int _AppHttpdStart() {
    OSStatus err = kNoErr;
    http_log("initializing web-services");

    /*Initialize HTTPD*/
    if (is_http_init == false) {
        err = httpd_init();
        require_noerr_action(err, exit, http_log("failed to initialize httpd"));
        is_http_init = true;
    }

    /*Start http thread*/
    err = httpd_start();
    if (err != kNoErr) {
        http_log("failed to start httpd thread");
        httpd_shutdown();
    }
exit:
    return err;
}

int AppHttpdStart(void) {
    OSStatus err = kNoErr;

    err = _AppHttpdStart();
    require_noerr(err, exit);

    if (is_handlers_registered == false) {
        AppHttpRegisterHandlers();
        is_handlers_registered = true;
    }

exit:
    return err;
}

int AppHttpdStop() {
    OSStatus err = kNoErr;

    /* HTTPD and services */
    http_log("stopping down httpd");
    err = httpd_stop();
    require_noerr_action(err, exit, http_log("failed to halt httpd"));

exit:
    return err;
}
