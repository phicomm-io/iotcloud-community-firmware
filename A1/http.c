#include "../common/ota/mode.h"
#include "../common/watchdog.h"
#include "../common/service.h"
#include "a1.h"
#include "filter.h"
#include "../common/ota/signed_ota.h"
#include "web_data.h"
#include <ctype.h>
#include <errno.h>
#include "../common/runtime_metrics.h"
#include <limits.h>
#include "printbuf.h"

#define HTTP_PENDING_CLIENTS 8u
#define HTTP_TCP_CLIENT_LIMIT 24
#define HTTP_RESPONSE_TIMEOUT_MS 10000u
#define HTTP_SEND_CHUNK_SIZE 1024u

typedef struct {
    char method[8], path[96], authorization[128];
    uint32_t length;
    bool have_length, chunked;
} request_t;

static volatile uint32_t http_accept_ok;
static volatile uint32_t http_accept_fail;
static volatile int http_accept_errno;
static volatile int http_accept_last_fd = -1;
static volatile uint32_t http_accept_queue_ok;
static volatile uint32_t http_accept_queue_reject;
static volatile int http_accept_queue_last_rc = -1;
static volatile uint32_t http_accept_busy_fcntl_fail;
static volatile uint32_t http_accept_busy_drained;
static volatile uint32_t http_accept_busy_send_ok;
static volatile uint32_t http_accept_busy_send_fail;
static volatile int http_accept_busy_send_errno;
static volatile int http_accept_busy_send_bytes;
static volatile uint32_t http_accept_overflow_close;

static void http_accept_count(volatile uint32_t *counter)
{
    if (*counter < (uint32_t)INT_MAX) *counter += 1;
}

typedef struct {
    int set_rc, set_errno, limit_get_rc, limit, connections_get_rc, connections;
} http_diag_t;

static http_diag_t http_diag = {-2, 0, -2, 0, -2, 0};

static void http_diag_sample(int listener, int set_rc, int set_errno, bool read_limit)
{
    http_diag_t sample = {set_rc, set_errno, -2, 0, -1, 0};
    socklen_t option_length;
    if (read_limit) {
        option_length = sizeof(sample.limit);
        sample.limit_get_rc = getsockopt(listener, IPPROTO_TCP, TCP_MAX_CONN_NUM, &sample.limit, &option_length);
    }
    option_length = sizeof(sample.connections);
    sample.connections_get_rc = getsockopt(listener, IPPROTO_TCP, TCP_CONN_NUM, &sample.connections, &option_length);
    mico_rtos_lock_mutex(&a1_lock);
    if (!read_limit) { sample.limit_get_rc = http_diag.limit_get_rc; sample.limit = http_diag.limit; }
    http_diag = sample;
    mico_rtos_unlock_mutex(&a1_lock);
}

static http_diag_t http_diag_snapshot(void)
{
    http_diag_t snapshot;
    mico_rtos_lock_mutex(&a1_lock);
    snapshot = http_diag;
    mico_rtos_unlock_mutex(&a1_lock);
    return snapshot;
}

int a1_socket_read(int fd, void *data, size_t n, unsigned timeout)
{
    fd_set readfds; FD_ZERO(&readfds); FD_SET(fd, &readfds);
    struct timeval tv = { timeout / 1000, (timeout % 1000) * 1000 };
    if (select(fd + 1, &readfds, NULL, NULL, &tv) <= 0) return -1;
    return recv(fd, data, n, 0);
}

bool a1_socket_send(int fd, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        fd_set writefds; FD_ZERO(&writefds); FD_SET(fd, &writefds);
        struct timeval tv = {10, 0};
        if (select(fd + 1, NULL, &writefds, NULL, &tv) <= 0) return false;
        int sent = send(fd, p, n, 0);
        if (sent <= 0) return false;
        p += sent; n -= sent;
    }
    return true;
}

int a1_socket_line(int fd, char *line, size_t capacity)
{
    uint32_t deadline = mico_rtos_get_time() + 10000;
    size_t used = 0;
    while (used + 1 < capacity) {
        uint32_t now = mico_rtos_get_time();
        if ((int32_t)(now - deadline) >= 0) return -1;
        char c;
        if (a1_socket_read(fd, &c, 1, deadline - now) != 1 || !c) return -1;
        if (c == '\n') {
            if (!used || line[used - 1] != '\r') return -1;
            line[--used] = 0; return used;
        }
        line[used++] = c;
    }
    return -1;
}

static bool response_send_until(int fd, const void *data, size_t length, uint32_t deadline)
{
    const uint8_t *p = data;
    while (length) {
        uint32_t now = mico_rtos_get_time();
        if ((int32_t)(now - deadline) >= 0) return false;
        unsigned remaining = deadline - now;
        fd_set writable; FD_ZERO(&writable); FD_SET(fd, &writable);
        struct timeval tv = {remaining / 1000, (remaining % 1000) * 1000};
        int ready = select(fd + 1, NULL, &writable, NULL, &tv);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (int32_t)(mico_rtos_get_time() - deadline) >= 0) return false;
        /* A writable MiCO socket need not have room for the entire page.
         * Bound each enqueue so a large response can make forward progress. */
        size_t chunk = length < HTTP_SEND_CHUNK_SIZE ? length : HTTP_SEND_CHUNK_SIZE;
        int sent = send(fd, p, chunk, 0);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (sent <= 0) return false;
        p += sent; length -= sent;
    }
    return true;
}

static bool response_send(int fd, const void *header, size_t header_length, const void *body, size_t length)
{
    /* These inbound sockets close after the response. Nonblocking send keeps
     * one shared header/body deadline effective even after select says ready. */
    uint32_t deadline = mico_rtos_get_time() + HTTP_RESPONSE_TIMEOUT_MS;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) return false;
    return response_send_until(fd, header, header_length, deadline) &&
           response_send_until(fd, body, length, deadline);
}

static bool response(int fd, unsigned code, const char *type, const void *body, size_t length, bool gzip)
{
    char header[320];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\n%s%s\r\n", code,
        code == 200 ? "OK" : code == 202 ? "Accepted" : code == 401 ? "Unauthorized" : "Error",
        type, (unsigned)length, gzip ? "Content-Encoding: gzip\r\n" : "",
        code == 401 ? "WWW-Authenticate: Basic realm=\"A1\"\r\n" : "");
    return response_send(fd, header, strlen(header), body, length);
}

static void message(int fd, unsigned code, const char *text)
{
    json_object *o = json_object_new_object();
    if (!o) return;
    json_build_object_add(o, "message", json_object_new_string(text));
    const char *body = json_object_to_json_string(o);
    if (!body) {
        static const char busy[] = "{\"code\":\"busy\"}";
        response(fd, 503, "application/json", busy, sizeof(busy) - 1, false);
        json_object_put(o); return;
    }
    response(fd, code, "application/json", body, strlen(body), false); json_object_put(o);
}

static bool read_request(int fd, request_t *req)
{
    char line[256], version[16], extra; unsigned budget = 0;
    memset(req, 0, sizeof(*req));
    int n = a1_socket_line(fd, line, sizeof(line));
    if (n < 0 || sscanf(line, "%7s %95s %15s %c", req->method, req->path, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) return false;
    uint32_t deadline = mico_rtos_get_time() + 15000;
    for (;;) {
        if ((int32_t)(mico_rtos_get_time() - deadline) >= 0) return false;
        n = a1_socket_line(fd, line, sizeof(line));
        if (n < 0 || (budget += n + 2) > 4096) return false;
        if (!n) return !req->chunked;
        char *colon = strchr(line, ':');
        if (!colon) return false;
        *colon++ = 0; while (*colon == ' ' || *colon == '\t') colon++;
        if (!strcasecmp(line, "Content-Length")) {
            if (req->have_length || !isdigit((unsigned char)*colon)) return false;
            char *end; unsigned long length = strtoul(colon, &end, 10);
            if (*end || length > A1_APP_OFFSET + A1_APP_LIMIT + 16 + OTA_ENVELOPE_SIZE) return false;
            req->length = length; req->have_length = true;
        } else if (!strcasecmp(line, "Transfer-Encoding")) req->chunked = true;
        else if (!strcasecmp(line, "Authorization")) {
            if (strlen(colon) >= sizeof(req->authorization)) return false;
            strcpy(req->authorization, colon);
        } else if (!strcasecmp(line, "Expect")) return false;
    }
}

static bool authorized(const request_t *req, const char *password)
{
    if (!password[0]) return true;
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char credentials[40], expected[64] = "Basic ";
    snprintf(credentials, sizeof(credentials), "admin:%s", password);
    unsigned n = strlen(credentials), pos = 6;
    for (unsigned i = 0; i < n; i += 3) {
        uint32_t value = (uint32_t)(uint8_t)credentials[i] << 16;
        if (i + 1 < n) value |= (uint32_t)(uint8_t)credentials[i + 1] << 8;
        if (i + 2 < n) value |= (uint8_t)credentials[i + 2];
        expected[pos++] = alphabet[(value >> 18) & 63]; expected[pos++] = alphabet[(value >> 12) & 63];
        expected[pos++] = i + 1 < n ? alphabet[(value >> 6) & 63] : '=';
        expected[pos++] = i + 2 < n ? alphabet[value & 63] : '=';
    }
    expected[pos] = 0;
    if (strlen(req->authorization) != pos) return false;
    unsigned difference = 0;
    for (unsigned i = 0; i < pos; i++) difference |= (uint8_t)expected[i] ^ (uint8_t)req->authorization[i];
    return difference == 0;
}

static void str(json_object *o, const char *key, const char *value)
{ json_build_object_add(o, key, json_object_new_string(value)); }
static void integer(json_object *o, const char *key, int value)
{ json_build_object_add(o, key, json_object_new_int(value)); }
static void json_bool(json_object *o, const char *key, bool value)
{ json_build_object_add(o, key, json_object_new_boolean(value)); }
json_object *a1_status_json(void)
{
    a1_config_t cfg; a1_state_t s;
    a1_config_get(&cfg); a1_state_get(&s);
    json_object *o = json_object_new_object();
    if (!o) return NULL;
    runtime_metrics_json(o);
    http_diag_t diag = http_diag_snapshot();
    integer(o, "http_accept_ok", (int)http_accept_ok);
    integer(o, "http_accept_fail", (int)http_accept_fail);
    integer(o, "http_accept_errno", http_accept_errno);
    integer(o, "http_accept_last_fd", http_accept_last_fd);
    integer(o, "http_accept_queue_ok", (int)http_accept_queue_ok);
    integer(o, "http_accept_queue_reject", (int)http_accept_queue_reject);
    integer(o, "http_accept_queue_last_rc", http_accept_queue_last_rc);
    integer(o, "http_accept_busy_fcntl_fail", (int)http_accept_busy_fcntl_fail);
    integer(o, "http_accept_busy_drained", (int)http_accept_busy_drained);
    integer(o, "http_accept_busy_send_ok", (int)http_accept_busy_send_ok);
    integer(o, "http_accept_busy_send_fail", (int)http_accept_busy_send_fail);
    integer(o, "http_accept_busy_send_errno", http_accept_busy_send_errno);
    integer(o, "http_accept_busy_send_bytes", http_accept_busy_send_bytes);
    integer(o, "http_accept_overflow_close", (int)http_accept_overflow_close);
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
    watchdog_status_json(o);
    json_build_object_add(o, "filter", a1_filter_json());
    str(o, "version", A1_VERSION); str(o, "device_id", a1_id); str(o, "name", cfg.name);
    json_bool(o, "wifi_trial_active", s.wifi_trial_active);
    str(o, "wifi_trial_status", s.wifi_trial_status);
    str(o, "ssid", cfg.ssid); str(o, "ip", s.ip); str(o, "ap_name", s.ap_name);
    str(o, "mqtt_host", cfg.mqtt_host); integer(o, "mqtt_port", cfg.mqtt_port); str(o, "mqtt_user", cfg.mqtt_user);
    str(o, "mqtt_topic_mode", "tmcp");
    str(o, "mqtt_protocol", "tmcp"); integer(o, "mqtt_protocol_version", 2);
    str(o, "mqtt_status", mqtt_status_name(s.mqtt_status));
    integer(o, "mqtt_errors", s.mqtt_errors); str(o, "mqtt_last_error", s.mqtt_last_error);
    json_bool(o, "mqtt_cleanup_warning", s.mqtt_cleanup_warning);
    json_bool(o, "web_password_set", cfg.web_password[0] != 0);
    json_bool(o, "wifi_password_set", cfg.wifi_password[0] != 0);
    json_bool(o, "mqtt_password_set", cfg.mqtt_password[0] != 0);
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
    integer(o, "panel_errors", s.panel_errors); integer(o, "hardware_errors", s.hardware_errors);
    integer(o, "panel_last_error", s.panel_last_error);
    char raw[7];
    for (unsigned i = 0; i < 3; i++) snprintf(raw + 2 * i, 3, "%02x", s.panel_raw[i]);
    str(o, "panel_raw", raw); json_bool(o, "config_dirty", s.config_dirty); str(o, "save_error", s.save_error);
    json_bool(o,"ota_signature_verified",s.ota_signature_verified);
    json_bool(o,"ota_image_verified",s.ota_image_verified);
    integer(o,"ota_key_id",s.ota_key_id);integer(o,"ota_image_length",s.ota_image_length);
    str(o,"ota_release_counter",s.ota_release_counter);str(o,"ota_version",s.ota_version);
    str(o, "ota_state", s.ota_state); str(o, "ota_error", s.ota_error);
    integer(o, "ota_progress", s.ota_progress); json_bool(o, "ota_active", s.ota_active);
    json_bool(o, "reboot_pending", s.reboot_pending); json_bool(o, "scan_busy", s.scan_busy);
    return o;
}

static void send_json(int fd, json_object *o)
{
    if (!o) { message(fd, 500, "Out of memory"); return; }
    /* A slow socket must not retain the full status/config object graph while
     * another owner builds a status snapshot. The wire buffer owns its bytes. */
    struct printbuf *wire = json_object_to_json_string_ex(o);
    json_object_put(o);
    if (!wire) {
        static const char busy[] = "{\"code\":\"busy\"}";
        response(fd, 503, "application/json", busy, sizeof(busy) - 1, false);
        return;
    }
    response(fd, 200, "application/json", wire->buf, wire->bpos, false);
    printbuf_free(wire);
}

static json_object *read_json(int fd, uint32_t length)
{
    if (!length || length > A1_JSON_LIMIT) return NULL;
    char *body = malloc(length + 1);
    if (!body) return NULL;
    size_t received = 0;
    uint32_t deadline = mico_rtos_get_time() + 15000;
    while (received < length && (int32_t)(mico_rtos_get_time() - deadline) < 0) {
        int n = a1_socket_read(fd, body + received, length - received, 5000);
        if (n <= 0) break;
        received += n;
    }
    body[received] = 0;
    json_object *o = received == length && !memchr(body, 0, received) ? service_parse(body, received) : NULL;
    free(body);
    if (o && !json_object_is_type(o, json_type_object)) { json_object_put(o); o = NULL; }
    return o;
}

static bool get_string(json_object *o, const char *key, char *out, size_t capacity, bool keep_blank)
{
    json_object *v = json_object_object_get(o, key);
    if (!v) return true;
    if (!json_object_is_type(v, json_type_string)) return false;
    const char *value = json_object_get_string(v);
    if ((int)strlen(value) != json_object_get_string_len(v) || strlen(value) >= capacity || strpbrk(value, "\r\n")) return false;
    if (!keep_blank || value[0]) strcpy(out, value);
    return true;
}

static bool get_number(json_object *o, const char *key, int *out, int minimum, int maximum)
{
    json_object *v = json_object_object_get(o, key);
    if (!v || !json_object_is_type(v, json_type_int)) return false;
    int value = json_object_get_int(v);
    if (value < minimum || value > maximum) return false;
    *out = value; return true;
}

static void scan_response(int fd)
{
    a1_state_t state; a1_state_get(&state);
    json_object *o = json_object_new_object(), *list = json_object_new_array();
    if (!o || !list) {
        if (o) json_object_put(o);
        if (list) json_object_put(list);
        message(fd, 500, "Out of memory"); return;
    }
    json_bool(o, "scanning", state.scan_busy);
    for (unsigned i = 0; i < state.scan_count; i++) {
        json_object *ap = json_object_new_object();
        if (!ap) { json_object_put(list); json_object_put(o); message(fd, 500, "Out of memory"); return; }
        str(ap, "ssid", state.scan[i].ssid); integer(ap, "rssi", state.scan[i].rssi);
        json_build_array_add(list, ap);
    }
    json_build_object_add(o, "networks", list); send_json(fd, o);
}

static const char *feature_domain(const char *path)
{
    if (!strcmp(path, "/binding/config")) return "binding";
    if (!strcmp(path, "/binding/status")) return "status";
    if (!strcmp(path, "/binding/probe")) return "probe";
    if (!strcmp(path, "/linkage/config")) return "linkage";
    if (!strcmp(path, "/light/advanced")) return "light";
    if (!strcmp(path, "/schedule/config")) return "schedule";
    if (!strcmp(path, "/sleep/config")) return "sleep";
    if (!strcmp(path, "/time/config")) return "time";
    return NULL;
}

static void memory_response(int fd)
{
    /* Keep diagnostics usable when building the full status JSON fails.
     * Authentication is applied by handle() before reaching this route. */
    const micoMemInfo_t *memory = MicoGetMemoryInfo();
    if (!memory) {
        static const char unavailable[] = "{\"code\":\"unavailable\"}";
        response(fd, 503, "application/json", unavailable, sizeof(unavailable) - 1, false);
        return;
    }
    micoMemInfo_t snapshot = *memory;
    char body[256];
    int length = snprintf(body, sizeof(body),
        "{\"code\":\"ok\",\"data\":{\"free_memory\":%d,\"allocated_memory\":%d,"
        "\"free_memory_chunks\":%d,\"uptime_ms\":%lu}}",
        snapshot.free_memory, snapshot.allocted_memory, snapshot.num_of_chunks,
        (unsigned long)mico_rtos_get_time());
    if (length > 0 && (size_t)length < sizeof(body))
        response(fd, 200, "application/json", body, (size_t)length, false);
}

static void handle(int fd)
{
    request_t req; a1_config_t cfg;
    if (!read_request(fd, &req)) { message(fd, 400, "Invalid HTTP request"); return; }
    a1_config_get(&cfg);
    if (!authorized(&req, cfg.web_password)) { message(fd, 401, "Authentication required"); return; }
    if(strncmp(req.path,"/api/v1/",8) && strcmp(req.path,"/") && strcmp(req.path,"/index.html")){message(fd,404,"Not found");return;}
    if (!strncmp(req.path, "/api/v1/", 8)) {
        if (!strcmp(req.method, "GET")) {
            if (!strcmp(req.path, "/api/v1/memory")) { memory_response(fd); return; }
            const char *action = !strcmp(req.path,"/api/v1/status")?"status.get":!strcmp(req.path,"/api/v1/capabilities")?"capabilities.get":!strcmp(req.path,"/api/v1/config")?"config.get":!strcmp(req.path,"/api/v1/logs")?"logs.get":!strcmp(req.path,"/api/v1/schedules")?"schedules.get":NULL;
            if(action) send_json(fd,service_query(action)); else message(fd,404,"Not found");
            return;
        }
        /* Volatile, authenticated physical mapping aid. Slot 0 exits. */
        if (!strcmp(req.method,"POST") && !strcmp(req.path,"/api/v1/light/test")) {
            if(ota_job_active()){message(fd,409,"OTA in progress");return;}
            json_object *body = read_json(fd, req.length);
            json_object *slot = body ? json_object_object_get(body, "slot") : NULL;
            if (!slot || !json_object_is_type(slot, json_type_int) ||
                json_object_get_int(slot) < 0 || json_object_get_int(slot) > 32) {
                if (body) json_object_put(body);
                message(fd, 400, "slot must be 0 (exit) or 1..32"); return;
            }
            json_object *neighbors = json_object_object_get(body, "neighbors");
            if (neighbors && !json_object_is_type(neighbors, json_type_boolean)) {
                json_object_put(body); message(fd, 400, "neighbors must be boolean"); return;
            }
            unsigned n = json_object_get_int(slot);
            bool adjacent = neighbors && json_object_get_boolean(neighbors);
            mico_rtos_lock_mutex(&a1_lock);
            bool busy = a1_state.ota_active || a1_state.reboot_pending;
            if (!busy) {
                a1_state.light_test_slot = n;
                a1_state.light_test_neighbors = adjacent;
                a1_state.light_test_until = n ? mico_rtos_get_time() + 120000 : 0;
            }
            mico_rtos_unlock_mutex(&a1_lock);
            json_object_put(body);
            message(fd, busy ? 409 : 200, busy ? "Device busy" : "Light test updated");
            return;
        }
        if (!strcmp(req.method,"POST") && !strcmp(req.path,"/api/v1/command")) {
            if(ota_job_active() && req.length>1024){message(fd,409,"OTA in progress");return;}
            json_object *request=read_json(fd,req.length),*reply=service_command(request);
            if(request)json_object_put(request);send_json(fd,reply);return;
        }
        if (strcmp(req.path,"/api/v1/ota/upload") && strcmp(req.path,"/api/v1/ota/upload-unsigned")) { message(fd,404,"Not found");return; }
        strcpy(req.path,!strcmp(req.path,"/api/v1/ota/upload")?"/ota/fileUpload":"/ota/fileUploadUnsigned");
    }
    if (!strcmp(req.method,"GET") && (!strcmp(req.path,"/") || !strcmp(req.path,"/index.html"))) {
        response(fd,200,"text/html; charset=utf-8",a1_web_data,sizeof(a1_web_data),true);return;
    }
    if(strcmp(req.method,"POST")){message(fd,404,"Not found");return;}
    if (!strcmp(req.path, "/ota/fileUpload") || !strcmp(req.path, "/ota/fileUploadUnsigned")) {
        bool rescue=!strcmp(req.path,"/ota/fileUploadUnsigned");
        if (!req.have_length || !(rescue ? a1_ota_begin_unsigned(req.length) : a1_ota_begin(req.length))) { message(fd, 400, "Invalid OTA size or busy"); return; }
        uint8_t data[512]; uint32_t received = 0, deadline = mico_rtos_get_time() + 300000;
        while (received < req.length) {
            if ((int32_t)(mico_rtos_get_time() - deadline) >= 0) { a1_ota_abort("Upload timed out"); break; }
            unsigned want = req.length - received > sizeof(data) ? sizeof(data) : req.length - received;
            int n = a1_socket_read(fd, data, want, 15000);
            if (n <= 0) { a1_ota_abort("Upload interrupted"); break; }
            if (!a1_ota_write(data, n)) break;
            received += n;
        }
        if (received == req.length && a1_ota_finish()) message(fd, 202, "OTA verified; rebooting");
        else message(fd, 400, "OTA rejected; see /ota for details");
        return;
    }
    message(fd,404,"Not found");
}

/* Keep accept responsive while the single request worker reads a slow client.
 * MiCO's pending-connection mailbox is separate from TCP_MAX_CONN_NUM. */
static mico_queue_t http_pending;
static void http_worker(mico_thread_arg_t arg);

static void http_accept_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    /* MiCO requires each socket to be operated by one thread. The listener,
     * including its diagnostic getsockopt calls, belongs to this owner. */
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET;
    address.sin_port = htons(80); address.sin_addr.s_addr = INADDR_ANY;
    /* The SDK ignores listen's backlog argument; TCP_MAX_CONN_NUM is separate. */
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listener, 0) < 0) {
        if (listener >= 0) close(listener);
        custom_log("A1 HTTP", "Cannot listen on port 80"); mico_rtos_delete_thread(NULL); return;
    }
    int clients = HTTP_TCP_CLIENT_LIMIT;
    int set_rc = setsockopt(listener, IPPROTO_TCP, TCP_MAX_CONN_NUM, &clients, sizeof(clients));
    int set_errno = set_rc < 0 ? errno : 0;
    http_diag_sample(listener, set_rc, set_errno, true);
    if (set_rc < 0) custom_log("A1 HTTP", "Cannot raise TCP client limit");
    OSStatus err = mico_rtos_init_queue(&http_pending, "a1_http_pending", sizeof(int), HTTP_PENDING_CLIENTS);
    if (err != kNoErr) {
        close(listener);
        custom_log("A1 HTTP", "Cannot create pending client queue");
        mico_rtos_delete_thread(NULL); return;
    }
    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_http", http_worker, 8192, 0);
    if (err != kNoErr) {
        mico_rtos_deinit_queue(&http_pending);
        close(listener);
        custom_log("A1 HTTP", "Cannot create request worker");
        mico_rtos_delete_thread(NULL); return;
    }
    static const char busy[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: application/json\r\nContent-Length: 15\r\n"
        "Connection: close\r\n\r\n{\"code\":\"busy\"}";
    for (;;) {
        /* Bound queued TCP buffers during OTA; only this thread owns listener. */
        int wanted=ota_job_active()?4:HTTP_TCP_CLIENT_LIMIT;
        if(wanted!=clients){
            int rc=setsockopt(listener,IPPROTO_TCP,TCP_MAX_CONN_NUM,&wanted,sizeof(wanted));
            if(rc==0)clients=wanted;
            http_diag_sample(listener,rc,rc<0?errno:0,true);
        }
        int fd = accept(listener, NULL, NULL);
        int accept_error = fd < 0 ? errno : 0;
        if (fd < 0) {
            http_accept_count(&http_accept_fail);
            http_accept_errno = accept_error;
            mico_thread_msleep(100); continue;
        }
        http_accept_count(&http_accept_ok);
        http_accept_last_fd = fd;
        http_diag_sample(listener, set_rc, set_errno, false);
        OSStatus queue_rc = mico_rtos_push_to_queue(&http_pending, &fd, 0);
        http_accept_queue_last_rc = queue_rc;
        if (queue_rc == kNoErr) {
            http_accept_count(&http_accept_queue_ok);
            continue; /* The request worker now owns fd. */
        }
        http_accept_count(&http_accept_queue_reject);
        /* One absolute budget includes both sending and waiting for the client
         * to consume the Content-Length response and close. Late request bytes
         * must not race an immediate close. Never execute rejected requests. */
        if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0) {
            uint32_t deadline = mico_rtos_get_time() + 100;
            size_t sent_total = 0;
            int send_error = 0;
            while (sent_total < sizeof(busy) - 1) {
                uint32_t now = mico_rtos_get_time();
                if ((int32_t)(now - deadline) >= 0) break;
                fd_set writable; FD_ZERO(&writable); FD_SET(fd, &writable);
                unsigned remaining = deadline - now;
                struct timeval tv = {0, remaining * 1000};
                if (select(fd + 1, NULL, &writable, NULL, &tv) <= 0) break;
                int sent = send(fd, busy + sent_total, sizeof(busy) - 1 - sent_total, 0);
                if (sent <= 0) { send_error = sent < 0 ? errno : 0; break; }
                sent_total += (size_t)sent;
            }
            http_accept_busy_send_bytes = (int)sent_total;
            if (sent_total == sizeof(busy) - 1) http_accept_count(&http_accept_busy_send_ok);
            else {
                http_accept_count(&http_accept_busy_send_fail);
                http_accept_busy_send_errno = send_error;
            }
            char discard[128];
            /* Also bound work for a sender that continuously streams data. */
            for (unsigned reads = 0; reads < 64; reads++) {
                uint32_t now = mico_rtos_get_time();
                if ((int32_t)(now - deadline) >= 0) break;
                fd_set readable; FD_ZERO(&readable); FD_SET(fd, &readable);
                unsigned remaining = deadline - now;
                struct timeval tv = {0, remaining * 1000};
                if (select(fd + 1, &readable, NULL, NULL, &tv) <= 0) break;
                int received = recv(fd, discard, sizeof(discard), 0);
                if (received <= 0) break;
                http_accept_count(&http_accept_busy_drained);
            }
        } else {
            http_accept_count(&http_accept_busy_fcntl_fail);
        }
        http_accept_count(&http_accept_overflow_close);
        close(fd);
    }
}

static void http_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    for (;;) {
        int fd;
        if (mico_rtos_pop_from_queue(&http_pending, &fd, MICO_WAIT_FOREVER) != kNoErr) {
            mico_thread_msleep(100); continue;
        }
        handle(fd);
        close(fd);
    }
}

OSStatus a1_http_start(void)
{ return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_http_accept", http_accept_worker, 2048, 0); }
