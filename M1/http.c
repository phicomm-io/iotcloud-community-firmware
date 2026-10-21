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
#include "../common/runtime_metrics.h"
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

static __attribute__((noinline)) json_object *m1_status_json_view(bool live)
{
    m1_config_t cfg; m1_state_t s;
    m1_config_get(&cfg); m1_state_get(&s);
    uint32_t now = mico_rtos_get_time(); bool fresh = m1_sample_fresh(&s.sample, now);
    json_object *o = json_object_new_object();
    if (!o) return NULL;
    str(o,"model","m1");
    if (!live) {
        phicomm_variant_json(o);
        runtime_metrics_json(o);
        http_diag_t diag = http_diag_snapshot();
        integer(o, "http_accept_ok", (int)diag.accepted);
        integer(o, "http_accept_fail", (int)diag.accept_failures);
        integer(o, "http_accept_errno", diag.accept_errno);
        integer(o, "http_accept_last_fd", diag.last_fd);
        integer(o, "http_accept_queue_ok", (int)diag.queued);
        integer(o, "http_accept_queue_reject", (int)diag.rejected);
        json_object_object_add(o,"http_accept_queue_last_rc",NULL);
        integer(o, "http_accept_busy_fcntl_fail", (int)diag.nonblock_failures);
        integer(o, "http_accept_busy_drained", (int)diag.drained);
        integer(o, "http_accept_busy_send_ok", (int)diag.busy_sent);
        integer(o, "http_accept_busy_send_fail", (int)diag.busy_failed);
        integer(o, "http_accept_busy_send_errno", diag.busy_errno);
        integer(o, "http_accept_busy_send_bytes", (int)diag.busy_bytes);
        integer(o, "http_accept_overflow_close", (int)diag.rejected);
        integer(o, "http_accept_queue_capacity", HTTP_PENDING_CLIENTS);
        integer(o, "http_tcp_limit_requested", HTTP_TCP_CLIENT_LIMIT);
        integer(o, "http_tcp_limit_set_rc", diag.set_rc);
        integer(o, "http_tcp_limit_set_errno", diag.set_errno);
        integer(o, "http_tcp_limit_get_rc", diag.limit_get_rc);
        if (diag.limit_get_rc == 0) json_build_object_add(o, "http_tcp_limit_readback",json_object_new_int(diag.limit));
        else json_object_object_add(o, "http_tcp_limit_readback", NULL);
        integer(o, "http_tcp_connections_get_rc", diag.connections_get_rc);
        if (diag.connections_get_rc == 0) json_build_object_add(o, "http_tcp_connections",json_object_new_int(diag.connections));
        else json_object_object_add(o, "http_tcp_connections", NULL);
        integer(o,"http_core_owner_state",diag.owner_state);
        integer(o,"http_core_restarts",diag.restarts);
        integer(o,"http_core_close_failures",diag.close_failures);
        integer(o,"http_core_last_close_rc",diag.last_close_rc);
        integer(o,"http_core_last_close_errno",diag.last_close_errno);
        integer(o,"http_core_slot_bytes",diag.slot_bytes);
        integer(o,"http_core_server_bytes",diag.server_bytes);
        watchdog_status_json(o);
    } else integer(o, "uptime_seconds", mico_rtos_get_time()/1000);
    str(o, "version", M1_VERSION); str(o, "device_id", m1_id); str(o, "name", cfg.name);
    json_bool(o, "wifi_trial_active", s.wifi_trial_active);
    str(o, "wifi_trial_status", s.wifi_trial_status);
    if (!live) {
        str(o, "ssid", cfg.ssid); str(o, "ip", s.ip); str(o, "ap_name", s.ap_name);
        str(o, "mqtt_host", cfg.mqtt_host); integer(o, "mqtt_port", cfg.mqtt_port); str(o, "mqtt_user", cfg.mqtt_user);
        str(o, "mqtt_topic_mode", "tmcp");
        str(o, "mqtt_protocol", "tmcp"); integer(o, "mqtt_protocol_version", 2);
        integer(o, "mqtt_errors", s.mqtt_errors); str(o, "mqtt_last_error", s.mqtt_last_error);
        integer(o, "timezone_minutes", cfg.timezone_minutes);
        char ntp_host[64]; m1_ntp_host_get(ntp_host);
        str(o, "ntp_host", ntp_host);
        str(o, "ntp_status", s.ntp_status[0] ? s.ntp_status : "waiting_network");
        str(o, "ntp_server", s.ntp_server);
        json_bool(o, "ntp_synced", s.ntp_synced);
        json_bool(o, "ntp_screen_queued", s.ntp_screen_queued);
        /* This SDK serializes doubles with %g (six significant digits), which
         * rounds Unix timestamps by minutes. Preserve whole seconds as an integer. */
        if (s.ntp_synced) json_build_object_add(o, "ntp_last_success",json_object_new_int(s.ntp_last_success));
        else json_object_object_add(o, "ntp_last_success", NULL);
        integer(o, "ntp_next_seconds", s.wifi_connected && s.ntp_next_at &&
            (int32_t)(s.ntp_next_at - now) > 0 ? (uint32_t)(s.ntp_next_at - now) / 1000 : 0);
        json_bool(o, "mqtt_password_set", cfg.mqtt_password[0] != 0);
        json_bool(o, "web_password_set", cfg.web_password[0] != 0);
    }
    str(o, "mqtt_status", mqtt_status_name(s.mqtt_status));
    json_bool(o, "mqtt_cleanup_warning", s.mqtt_cleanup_warning);
    json_bool(o, "wifi_connected", s.wifi_connected); json_bool(o, "mqtt_connected", s.mqtt_connected);
    json_bool(o, "ap_active", s.ap_active); json_bool(o, "uart_ready", s.uart_ready);
    json_bool(o, "sensor_valid", fresh); json_bool(o, "sensor_received", s.sample.received);
    if (s.sample.received) json_build_object_add(o, "age_ms",json_object_new_double((uint32_t)(now - s.sample.updated_ms)));
    else json_object_object_add(o, "age_ms", NULL);
    json_object *co2 = json_object_new_object();
    bool co2_fresh = s.scd40.ready && s.scd40.received &&
        (uint32_t)(now - s.scd40.updated_ms) < 20000;
    json_bool(co2, "ready", s.scd40.ready); json_bool(co2, "valid", co2_fresh);
    str(co2, "serial", s.scd40.serial);
    integer(co2, "errors", s.scd40.errors);
    integer(co2, "last_error", s.scd40.last_error);
    if (!live) {
        integer(co2, "samples", s.scd40.samples);
        integer(co2, "crc_errors", s.scd40.crc_errors);
        json_bool(co2, "bus_initialized", s.scd40.bus_initialized);
        integer(co2, "last_command", s.scd40.last_command);
        integer(co2, "error_phase", s.scd40.error_phase);
    }
    if (s.scd40.received) integer(co2, "age_ms", (uint32_t)(now - s.scd40.updated_ms));
    else json_object_object_add(co2, "age_ms", NULL);
    if (co2_fresh) {
        integer(co2, "co2_ppm", s.scd40.co2_ppm);
    } else {
        json_object_object_add(co2, "co2_ppm", NULL);
    }
    json_build_object_add(o, "scd40", co2);
    integer(o, "brightness_requested", s.brightness_requested);
    if (s.sample.received) json_build_object_add(o, "temperature",json_object_new_double(s.sample.temperature_deci_c / 10.0));
    else json_object_object_add(o, "temperature", NULL);
    if (s.sample.received) json_build_object_add(o, "humidity",json_object_new_double(s.sample.humidity_deci_pct / 10.0));
    else json_object_object_add(o, "humidity", NULL);
    if (s.sample.received) json_build_object_add(o, "formaldehyde",json_object_new_double(s.sample.hcho_ug_m3 / 1000.0));
    else json_object_object_add(o, "formaldehyde", NULL);
    if (s.sample.received) json_build_object_add(o, "pm25",json_object_new_int(s.sample.pm25_ug_m3));
    else json_object_object_add(o, "pm25", NULL);
    if (!live) {
        integer(o, "rx_frames", s.rx_frames); integer(o, "unknown_frames", s.unknown_frames);
        integer(o, "uart_errors", s.uart_errors); integer(o, "discarded_bytes", s.discarded_bytes);
        char frame[41];
        for (unsigned i = 0; i < 20; i++) snprintf(frame + i * 2, 3, "%02x", s.last_frame[i]);
        str(o, "last_frame", frame); json_bool(o,"ota_signature_verified",s.ota_signature_verified);
        json_bool(o,"ota_image_verified",s.ota_image_verified);
        integer(o,"ota_key_id",s.ota_key_id);integer(o,"ota_image_length",s.ota_image_length);
        str(o,"ota_release_counter",s.ota_release_counter);str(o,"ota_version",s.ota_version);
    }
    str(o, "ota_state", s.ota_state); str(o, "ota_error", s.ota_error);
    integer(o, "ota_progress", s.ota_progress); json_bool(o, "ota_active", s.ota_active);
    json_bool(o, "scan_busy", s.scan_busy);
    return o;
}

/* Preserve the legacy full snapshot for existing callers and telemetry. */
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
    char *body=product.scalar;
    int length = snprintf(body, sizeof(product.scalar),
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
    if(length>0)length=ph_http_diag_append(body,sizeof(product.scalar),length,&d);
    if (length > 0 && (size_t)length < sizeof(product.scalar))
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
    ph_http_json(&product,x,service_memory_failures()==product.failures?service_command(request):NULL);
    if(request)json_object_put(request);
}
static void release(ph_http_exchange *x,bool sent)
{
    (void)x;(void)sent;if(upload_open)m1_ota_abort("Upload interrupted");upload_open=false;upload_pending=false;
    ph_http_product_release(&product);
}
static unsigned admission(void) { return (upload_pending || ota_job_active())?2:HTTP_TCP_CLIENT_LIMIT; }
static const ph_http_app app={begin,prepare,body_chunk,complete,release,admission};
OSStatus m1_http_start(void) { return ph_http_start(&app,HTTP_TCP_CLIENT_LIMIT); }
