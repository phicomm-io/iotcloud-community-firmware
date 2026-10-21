#include "../common/ota/mode.h"
#include "../common/watchdog.h"
#include "../common/service.h"
#include "a1.h"
#include "filter.h"
#include "../common/ota/signed_ota.h"
#include "web_data.h"
#include "../common/firmware_variant.h"
#include "../common/http_cors.h"
#include "favicon.h"
#include <ctype.h>
#include <errno.h>
#include "../common/runtime_metrics.h"
#include <limits.h>
#include "printbuf.h"
#include "json_heap.h"
#include "../common/httpd/product.h"


#define HTTP_PENDING_CLIENTS 1u
#define HTTP_TCP_CLIENT_LIMIT 3
#define http_diag_t ph_http_diagnostics
#define http_diag_snapshot ph_http_snapshot
static ph_http_product product;
static const char *query_action;
static enum { QUERY, COMMAND, LIGHT, UPLOAD, DIRECT } operation;
static bool upload_open;
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
static __attribute__((noinline)) json_object *a1_status_json_view(bool live)
{
    a1_config_t cfg; a1_state_t s;
    a1_config_get(&cfg); a1_state_get(&s);
    json_object *o = json_object_new_object();
    if (!o) return NULL;
    str(o,"model","a1");
    if (!live) {
        phicomm_variant_json(o);
        runtime_metrics_json(o);
        http_diag_t diag = http_diag_snapshot();
        integer(o, "http_accept_ok", (int)diag.accepted);
        integer(o, "http_accept_fail", (int)diag.accept_failures);
        integer(o, "http_accept_errno", diag.last_errno);
        integer(o, "http_accept_last_fd", diag.last_fd);
        integer(o, "http_accept_queue_ok", (int)diag.queued);
        integer(o, "http_accept_queue_reject", (int)diag.rejected);
        json_object_object_add(o,"http_accept_queue_last_rc",NULL);
        integer(o, "http_accept_busy_fcntl_fail", (int)diag.nonblock_failures);
        integer(o, "http_accept_busy_drained", (int)diag.drained);
        integer(o, "http_accept_busy_send_ok", (int)diag.busy_sent);
        integer(o, "http_accept_busy_send_fail", (int)diag.busy_failed);
        integer(o, "http_accept_busy_send_errno", diag.last_errno);
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
        json_build_object_add(o, "filter", a1_filter_json());
    } else integer(o, "uptime_seconds", mico_rtos_get_time()/1000);
    str(o, "version", A1_VERSION); str(o, "device_id", a1_id); str(o, "name", cfg.name);
    json_bool(o, "wifi_trial_active", s.wifi_trial_active);
    str(o, "wifi_trial_status", s.wifi_trial_status);
    if (!live) {
        str(o, "ssid", cfg.ssid); str(o, "ip", s.ip); str(o, "ap_name", s.ap_name);
        str(o, "mqtt_host", cfg.mqtt_host); integer(o, "mqtt_port", cfg.mqtt_port); str(o, "mqtt_user", cfg.mqtt_user);
        str(o, "mqtt_topic_mode", "tmcp");
        str(o, "mqtt_protocol", "tmcp"); integer(o, "mqtt_protocol_version", 2);
        integer(o, "mqtt_errors", s.mqtt_errors); str(o, "mqtt_last_error", s.mqtt_last_error);
        json_bool(o, "web_password_set", cfg.web_password[0] != 0);
        json_bool(o, "wifi_password_set", cfg.wifi_password[0] != 0);
        json_bool(o, "mqtt_password_set", cfg.mqtt_password[0] != 0);
    }
    str(o, "mqtt_status", mqtt_status_name(s.mqtt_status));
    json_bool(o, "mqtt_cleanup_warning", s.mqtt_cleanup_warning);
    json_bool(o, "wifi_connected", s.wifi_connected); json_bool(o, "mqtt_connected", s.mqtt_connected);
    bool effective_on; unsigned effective_speed; feature_effective_fan(cfg.fan_on,cfg.speed,&effective_on,&effective_speed);
    str(o,"fan_mode",feature_fan_auto()?"auto":"manual");
    str(o,"fan_state",!effective_on?"off":effective_speed?"running":"idle");
    integer(o,"auto_max_speed",feature_fan_limit());
    json_bool(o,"requested_fan_on",cfg.fan_on); integer(o,"requested_speed",cfg.speed);
    json_bool(o, "ap_active", s.ap_active); json_bool(o, "fan_on", effective_on);
    integer(o, "speed", cfg.speed); integer(o, "percentage", effective_on ? effective_speed : 0);
    integer(o, "applied_speed", s.applied_speed); json_bool(o, "fan_ready", s.fan_ready);
    json_bool(o, "touch_active", s.touch_active);
    json_bool(o, "led_enabled", cfg.led_enabled); json_bool(o, "led_visible", s.led_visible);
    json_bool(o, "led_always_on", cfg.led_always_on); integer(o, "brightness", cfg.brightness);
    integer(o, "red", cfg.red); integer(o, "green", cfg.green); integer(o, "blue", cfg.blue);
    json_bool(o, "child_lock", cfg.child_lock); json_bool(o, "panel_ready", s.panel_ready);
    if (!live) {
        integer(o, "panel_errors", s.panel_errors); integer(o, "hardware_errors", s.hardware_errors);
        integer(o, "panel_last_error", s.panel_last_error);
        char raw[7];
        for (unsigned i = 0; i < 3; i++) snprintf(raw + 2 * i, 3, "%02x", s.panel_raw[i]);
        str(o, "panel_raw", raw);
    }
    json_bool(o, "config_dirty", s.config_dirty); str(o, "save_error", s.save_error);
    if (!live) {
        json_bool(o,"ota_signature_verified",s.ota_signature_verified);
        json_bool(o,"ota_image_verified",s.ota_image_verified);
        integer(o,"ota_key_id",s.ota_key_id);integer(o,"ota_image_length",s.ota_image_length);
        str(o,"ota_release_counter",s.ota_release_counter);str(o,"ota_version",s.ota_version);
    }
    str(o, "ota_state", s.ota_state); str(o, "ota_error", s.ota_error);
    integer(o, "ota_progress", s.ota_progress); json_bool(o, "ota_active", s.ota_active);
    json_bool(o, "reboot_pending", s.reboot_pending); json_bool(o, "scan_busy", s.scan_busy);
    return o;
}

/* Preserve the legacy full snapshot for existing callers and telemetry. */
json_object *a1_status_json(void) { return a1_status_json_view(false); }
json_object *a1_status_live_json(void) { return a1_status_json_view(true); }

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
        "\"json_live_blocks\":%lu,\"json_peak_blocks\":%lu,\"json_alloc_failures\":%lu,\"json_guard_denied\":%lu,"
        "\"mqtt_pending\":%lu,\"mqtt_peak\":%lu,\"mqtt_overflow\":%lu,\"mqtt_max_delay_ms\":%lu}}",
        snapshot.free_memory, snapshot.allocted_memory, snapshot.num_of_chunks,
        (unsigned long)mico_rtos_get_time(), SERVICE_MEMORY_RESERVE,
        (unsigned long)budget.reserved, (unsigned long)budget.active,
        (unsigned long)budget.waiting, (unsigned long)budget.denied,
        (unsigned long)budget.minimum_free,
        (unsigned long)budget.json_live, (unsigned long)budget.json_peak,
        (unsigned long)budget.json_failed, (unsigned long)budget.json_denied,
        (unsigned long)budget.mqtt_pending, (unsigned long)budget.mqtt_peak,
        (unsigned long)budget.mqtt_overflow, (unsigned long)budget.mqtt_max_delay);
    if (length > 0 && (size_t)length < sizeof(product.scalar))
        response(x, 200, "application/json", body, (size_t)length, false);
}


static void light_command(ph_http_exchange *x,json_object *body)
{
    json_object *slot=body?json_object_object_get(body,"slot"):NULL;
    if(!slot || !json_object_is_type(slot,json_type_int) || json_object_get_int(slot)<0 || json_object_get_int(slot)>32) {
        ph_http_message(&product,x,400,"slot must be 0 (exit) or 1..32");return;
    }
    json_object *neighbors=json_object_object_get(body,"neighbors");
    if(neighbors && !json_object_is_type(neighbors,json_type_boolean)) {
        ph_http_message(&product,x,400,"neighbors must be boolean");return;
    }
    unsigned n=json_object_get_int(slot);bool adjacent=neighbors && json_object_get_boolean(neighbors);
    mico_rtos_lock_mutex(&a1_lock);
    bool busy=a1_state.ota_active || a1_state.reboot_pending;
    if(!busy) { a1_state.light_test_slot=n;a1_state.light_test_neighbors=adjacent;a1_state.light_test_until=n?mico_rtos_get_time()+120000:0; }
    mico_rtos_unlock_mutex(&a1_lock);
    ph_http_message(&product,x,busy?409:200,busy?"Device busy":"Light test updated");
}
static ph_http_action prepare(ph_http_exchange *x)
{
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
    a1_config_t cfg;a1_config_get(&cfg);
    if(!ph_http_basic(r->authorization,"admin",cfg.web_password)) {
        ph_http_message(&product,x,401,"Authentication required");x->response.realm="A1";return PH_HTTP_REPLY;
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
        if(r->length>A1_JSON_LIMIT) { ph_http_message(&product,x,400,"Invalid JSON size");return PH_HTTP_REPLY; }
        return prepare(x);
    }
    if(!strcmp(r->method,"POST") && (!strcmp(r->path,"/api/v1/ota/upload") || !strcmp(r->path,"/api/v1/ota/upload-unsigned"))) {
        bool rescue=!strcmp(r->path,"/api/v1/ota/upload-unsigned");
        if(!r->have_length || !(rescue?a1_ota_begin_unsigned(r->length):a1_ota_begin(r->length))) {
            ph_http_message(&product,x,400,"Invalid OTA size or busy");return PH_HTTP_REPLY;
        }
        operation=UPLOAD;upload_open=true;x->body_idle_ms=15000;x->body_total_ms=300000;return PH_HTTP_READ_BODY;
    }
    ph_http_message(&product,x,404,"Not found");return PH_HTTP_REPLY;
}
static bool body_chunk(ph_http_exchange *x,const void *data,size_t n)
{
    if(operation==UPLOAD) {
        if(a1_ota_write(data,n))return true;
        ph_http_message(&product,x,400,"OTA rejected; see /ota for details");return false;
    }
    return ph_http_body_copy(&product,data,n);
}
static void complete(ph_http_exchange *x)
{
    if(operation==UPLOAD) {
        bool ok=a1_ota_finish();upload_open=false;
        ph_http_message(&product,x,ok?202:400,ok?"OTA verified; rebooting":"OTA rejected; see /ota for details");return;
    }
    json_object *request=ph_http_body_json(&product,x);
    if(operation==LIGHT)light_command(x,request);
    else ph_http_json(&product,x,service_memory_failures()==product.failures?service_command(request):NULL);
    if(request)json_object_put(request);
}
static void release(ph_http_exchange *x,bool sent)
{
    (void)x;(void)sent;if(upload_open)a1_ota_abort("Upload interrupted");upload_open=false;
    ph_http_product_release(&product);
}
static unsigned admission(void) { return ota_job_active()?2:HTTP_TCP_CLIENT_LIMIT; }
static const ph_http_app app={begin,prepare,body_chunk,complete,release,admission};
OSStatus a1_http_start(void) { return ph_http_start(&app,HTTP_TCP_CLIENT_LIMIT,true); }
