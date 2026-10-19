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
#include "../common/memory_wait.h"

static void query_response(int fd, const char *action);
static void command_response(int fd, uint32_t length);
static void memory_busy(int fd);

#define HTTP_PENDING_CLIENTS 4u
#define HTTP_TCP_CLIENT_LIMIT 8
#define HTTP_RESPONSE_TIMEOUT_MS 10000u
#define HTTP_SEND_CHUNK_SIZE 1024u

typedef struct {
    char method[8], path[96], authorization[128];
    char if_none_match[80], origin[160];
    uint32_t length;
    bool have_length, chunked;
} request_t;

/* Only the serial HTTP worker uses response headers; accept rejects use raw sends. */
static const char *response_cors = "";

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
    mico_rtos_lock_mutex(&m1_lock);
    if (!read_limit) { sample.limit_get_rc = http_diag.limit_get_rc; sample.limit = http_diag.limit; }
    http_diag = sample;
    mico_rtos_unlock_mutex(&m1_lock);
}

static http_diag_t http_diag_snapshot(void)
{
    http_diag_t snapshot;
    mico_rtos_lock_mutex(&m1_lock);
    snapshot = http_diag;
    mico_rtos_unlock_mutex(&m1_lock);
    return snapshot;
}

int m1_socket_read(int fd, void *data, size_t n, unsigned timeout)
{
    fd_set readfds; FD_ZERO(&readfds); FD_SET(fd, &readfds);
    struct timeval tv = { timeout / 1000, (timeout % 1000) * 1000 };
    if (select(fd + 1, &readfds, NULL, NULL, &tv) <= 0) return -1;
    return recv(fd, data, n, 0);
}

bool m1_socket_send(int fd, const void *data, size_t n)
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

int m1_socket_line(int fd, char *line, size_t capacity)
{
    uint32_t deadline = mico_rtos_get_time() + 10000;
    size_t used = 0;
    while (used + 1 < capacity) {
        uint32_t now = mico_rtos_get_time();
        if ((int32_t)(now - deadline) >= 0) return -1;
        char c;
        if (m1_socket_read(fd, &c, 1, deadline - now) != 1 || !c) return -1;
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
    /* Small bounded writes otherwise can wait for Nagle/delayed ACK cycles.
     * Keep the existing send deadline even if this optional setting fails. */
    int no_delay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    return response_send_until(fd, header, header_length, deadline) &&
           response_send_until(fd, body, length, deadline);
}

static bool response(int fd, unsigned code, const char *type, const void *body, size_t length, bool gzip)
{
    char header[768];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\n%s%s%s\r\n", code,
        code == 200 ? "OK" : code == 202 ? "Accepted" : code == 401 ? "Unauthorized" : "Error",
        type, (unsigned)length, gzip ? "Content-Encoding: gzip\r\n" : "",
        response_cors, code == 401 ? "WWW-Authenticate: Basic realm=\"M1\"\r\n" : "");
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
    int n = m1_socket_line(fd, line, sizeof(line));
    if (n < 0 || sscanf(line, "%7s %95s %15s %c", req->method, req->path, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) return false;
    uint32_t deadline = mico_rtos_get_time() + 15000;
    for (;;) {
        if ((int32_t)(mico_rtos_get_time() - deadline) >= 0) return false;
        n = m1_socket_line(fd, line, sizeof(line));
        if (n < 0 || (budget += n + 2) > 4096) return false;
        if (!n) return !req->chunked;
        char *colon = strchr(line, ':');
        if (!colon) return false;
        *colon++ = 0; while (*colon == ' ' || *colon == '\t') colon++;
        if (!strcasecmp(line, "Content-Length")) {
            if (req->have_length || !isdigit((unsigned char)*colon)) return false;
            char *end; unsigned long length = strtoul(colon, &end, 10);
            if (*end || length > M1_APP_OFFSET + M1_APP_LIMIT + 16 + OTA_ENVELOPE_SIZE) return false;
            req->length = length; req->have_length = true;
        } else if (!strcasecmp(line, "Transfer-Encoding")) req->chunked = true;
        else if (!strcasecmp(line, "If-None-Match")) {
            /* Ignore validators too long for our generated SHA-256 tag. */
            if (strlen(colon) < sizeof(req->if_none_match)) strcpy(req->if_none_match, colon);
        }
        else if (!strcasecmp(line, "Authorization")) {
            if (strlen(colon) >= sizeof(req->authorization)) return false;
            strcpy(req->authorization, colon);
        } else if (!strcasecmp(line, "Origin")) {
            if (strlen(colon) >= sizeof(req->origin)) return false;
            strcpy(req->origin, colon);
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

json_object *m1_status_json(void)
{
    m1_config_t cfg; m1_state_t s;
    m1_config_get(&cfg); m1_state_get(&s);
    uint32_t now = mico_rtos_get_time(); bool fresh = m1_sample_fresh(&s.sample, now);
    json_object *o = json_object_new_object();
    if (!o) return NULL;
    phicomm_variant_json(o);
    str(o,"model","m1");
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
    str(o, "version", M1_VERSION); str(o, "device_id", m1_id); str(o, "name", cfg.name);
    json_bool(o, "wifi_trial_active", s.wifi_trial_active);
    str(o, "wifi_trial_status", s.wifi_trial_status);
    str(o, "ssid", cfg.ssid); str(o, "ip", s.ip); str(o, "ap_name", s.ap_name);
    str(o, "mqtt_host", cfg.mqtt_host); integer(o, "mqtt_port", cfg.mqtt_port); str(o, "mqtt_user", cfg.mqtt_user);
    str(o, "mqtt_topic_mode", "tmcp");
    str(o, "mqtt_protocol", "tmcp"); integer(o, "mqtt_protocol_version", 2);
    str(o, "mqtt_status", mqtt_status_name(s.mqtt_status));
    integer(o, "mqtt_errors", s.mqtt_errors); str(o, "mqtt_last_error", s.mqtt_last_error);
    json_bool(o, "mqtt_cleanup_warning", s.mqtt_cleanup_warning);
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
    integer(co2, "samples", s.scd40.samples); integer(co2, "errors", s.scd40.errors);
    integer(co2, "crc_errors", s.scd40.crc_errors);
    integer(co2, "last_error", s.scd40.last_error);
    json_bool(co2, "bus_initialized", s.scd40.bus_initialized);
    integer(co2, "last_command", s.scd40.last_command);
    integer(co2, "error_phase", s.scd40.error_phase);
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
    integer(o, "rx_frames", s.rx_frames); integer(o, "unknown_frames", s.unknown_frames);
    integer(o, "uart_errors", s.uart_errors); integer(o, "discarded_bytes", s.discarded_bytes);
    char frame[41];
    for (unsigned i = 0; i < 20; i++) snprintf(frame + i * 2, 3, "%02x", s.last_frame[i]);
    str(o, "last_frame", frame); json_bool(o,"ota_signature_verified",s.ota_signature_verified);
    json_bool(o,"ota_image_verified",s.ota_image_verified);
    integer(o,"ota_key_id",s.ota_key_id);integer(o,"ota_image_length",s.ota_image_length);
    str(o,"ota_release_counter",s.ota_release_counter);str(o,"ota_version",s.ota_version);
    str(o, "ota_state", s.ota_state); str(o, "ota_error", s.ota_error);
    integer(o, "ota_progress", s.ota_progress); json_bool(o, "ota_active", s.ota_active);
    json_bool(o, "scan_busy", s.scan_busy);
    return o;
}

static void send_json(int fd, json_object *o)
{
    if (!o) {
        static const char busy[] = "{\"code\":\"busy\"}";
        response(fd, 503, "application/json", busy, sizeof(busy)-1, false); return;
    }
    const char *body = json_object_to_json_string(o);
    if (!body) {
        static const char busy[] = "{\"code\":\"busy\"}";
        response(fd, 503, "application/json", busy, sizeof(busy) - 1, false);
        json_object_put(o); return;
    }
    response(fd, 200, "application/json", body, strlen(body), false); json_object_put(o);
}

static json_object *read_json(int fd, uint32_t length)
{
    if (!length || length > M1_JSON_LIMIT) return NULL;
    char *body = malloc(length + 1);
    if (!body) return NULL;
    size_t received = 0;
    uint32_t deadline = mico_rtos_get_time() + 15000;
    while (received < length && (int32_t)(mico_rtos_get_time() - deadline) < 0) {
        int n = m1_socket_read(fd, body + received, length - received, 5000);
        if (n <= 0) break;
        received += n;
    }
    body[received] = 0;
    json_object *o = received == length && !memchr(body, 0, received) ? service_parse(body,received) : NULL;
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
    if (strlen(value) != (size_t)json_object_get_string_len(v) || strlen(value) >= capacity || strpbrk(value, "\r\n")) return false;
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
    m1_state_t state; m1_state_get(&state);
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
    if (!strcmp(path, "/binding/time/config")) return "time";
    return NULL;
}

static bool asset_response(int fd, const char *type, const void *body, size_t length,
                           bool gzip, const char *etag, const char *if_none_match)
{
    char header[768];
    /* Private caches must revalidate, including the normal authentication
     * check in handle(). The tag identifies the actual compressed bytes. */
    bool unchanged = !strcmp(etag, if_none_match) ||
        (!strncmp(if_none_match, "W/", 2) && !strcmp(etag, if_none_match + 2));
    if (unchanged) {
        snprintf(header, sizeof(header),
            "HTTP/1.1 304 Not Modified\r\nConnection: close\r\n"
            "Cache-Control: private, no-cache\r\nVary: Authorization\r\nETag: %s\r\n\r\n", etag);
        return response_send(fd, header, strlen(header), NULL, 0);
    }
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n"
        "Cache-Control: private, no-cache\r\nVary: Authorization\r\nETag: %s\r\n%s\r\n",
        type, (unsigned)length, etag, gzip ? "Content-Encoding: gzip\r\n" : "");
    return response_send(fd, header, strlen(header), body, length);
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
    service_memory_state_t budget; service_memory_snapshot(&budget);
    char body[512];
    int length = snprintf(body, sizeof(body),
        "{\"code\":\"ok\",\"data\":{\"free_memory\":%d,\"allocated_memory\":%d,"
        "\"free_memory_chunks\":%d,\"uptime_ms\":%lu,\"memory_reserve\":%u,"
        "\"memory_reserved\":%lu,\"memory_active\":%lu,\"memory_waiting\":%lu,"
        "\"memory_denied\":%lu,\"minimum_sampled_free\":%lu}}",
        snapshot.free_memory, snapshot.allocted_memory, snapshot.num_of_chunks,
        (unsigned long)mico_rtos_get_time(), SERVICE_MEMORY_RESERVE,
        (unsigned long)budget.reserved, (unsigned long)budget.active,
        (unsigned long)budget.waiting, (unsigned long)budget.denied,
        (unsigned long)budget.minimum_free);
    if (length > 0 && (size_t)length < sizeof(body))
        response(fd, 200, "application/json", body, (size_t)length, false);
}

static void handle(int fd)
{
    request_t req; m1_config_t cfg;
    response_cors = "";
    if (!read_request(fd, &req)) { message(fd, 400, "Invalid HTTP request"); return; }
    response_cors = phicomm_cors_headers(req.origin);
    if (!strcmp(req.method,"OPTIONS")) { response(fd, *response_cors ? 200 : 403, "text/plain", "", 0, false); return; }
    m1_config_get(&cfg);
    if (!authorized(&req, cfg.web_password)) { message(fd, 401, "Authentication required"); return; }
    if(strncmp(req.path,"/api/v1/",8) && strcmp(req.path,"/") && strcmp(req.path,"/index.html") && strcmp(req.path,"/favicon.ico")){message(fd,404,"Not found");return;}
    if (!strncmp(req.path, "/api/v1/", 8)) {
        if (!strcmp(req.method, "GET")) {
            if (!strcmp(req.path, "/api/v1/memory")) { memory_response(fd); return; }
            const char *action = !strcmp(req.path,"/api/v1/info")?"info.get":!strcmp(req.path,"/api/v1/status")?"status.get":!strcmp(req.path,"/api/v1/capabilities")?"capabilities.get":!strcmp(req.path,"/api/v1/config")?"config.get":!strcmp(req.path,"/api/v1/logs")?"logs.get":!strcmp(req.path,"/api/v1/schedules")?"schedules.get":NULL;
            if(action) query_response(fd,action); else message(fd,404,"Not found");
            return;
        }
        if (!strcmp(req.method,"POST") && !strcmp(req.path,"/api/v1/command")) {
            if(ota_job_active() && req.length>1024){message(fd,409,"OTA in progress");return;}
            command_response(fd,req.length);return;
        }
        if (strcmp(req.path,"/api/v1/ota/upload") && strcmp(req.path,"/api/v1/ota/upload-unsigned")) { message(fd,404,"Not found");return; }
        strcpy(req.path,!strcmp(req.path,"/api/v1/ota/upload")?"/ota/fileUpload":"/ota/fileUploadUnsigned");
    }
    if (!strcmp(req.method,"GET") && (!strcmp(req.path,"/") || !strcmp(req.path,"/index.html"))) {
        asset_response(fd,"text/html; charset=utf-8",m1_web_data,sizeof(m1_web_data),true,M1_WEB_ETAG,req.if_none_match);return;
    }
    if (!strcmp(req.method,"GET") && !strcmp(req.path,"/favicon.ico")) {
        asset_response(fd,"image/x-icon",m1_favicon_data,sizeof(m1_favicon_data),false,M1_FAVICON_ETAG,req.if_none_match);return;
    }
    if(strcmp(req.method,"POST")){message(fd,404,"Not found");return;}
    if (!strcmp(req.path, "/ota/fileUpload") || !strcmp(req.path, "/ota/fileUploadUnsigned")) {
        bool rescue=!strcmp(req.path,"/ota/fileUploadUnsigned");
        if (!req.have_length || !(rescue ? m1_ota_begin_unsigned(req.length) : m1_ota_begin(req.length))) { message(fd, 400, "Invalid OTA size or busy"); return; }
        uint8_t data[512]; uint32_t received = 0, deadline = mico_rtos_get_time() + 300000;
        while (received < req.length) {
            if ((int32_t)(mico_rtos_get_time() - deadline) >= 0) { m1_ota_abort("Upload timed out"); break; }
            unsigned want = req.length - received > sizeof(data) ? sizeof(data) : req.length - received;
            int n = m1_socket_read(fd, data, want, 15000);
            if (n <= 0) { m1_ota_abort("Upload interrupted"); break; }
            if (!m1_ota_write(data, n)) break;
            received += n;
        }
        if (received == req.length && m1_ota_finish()) message(fd, 202, "OTA verified; rebooting");
        else message(fd, 400, "OTA rejected; see /ota for details");
        return;
    }
    message(fd,404,"Not found");
}

/* Waiting retains only the accepted descriptor and stack metadata. The
 * already bounded FIFO remains owned by the single HTTP request worker. */
static void memory_busy(int fd)
{
    static const char busy[] = "{\"code\":\"busy\"}";
    response(fd,503,"application/json",busy,sizeof(busy)-1,false);
}
static void query_response(int fd, const char *action)
{
    unsigned claim=service_memory_wait(0);
    if(!claim){memory_busy(fd);return;}
    send_json(fd,service_query(action));service_memory_release(claim);
}
static void command_response(int fd, uint32_t length)
{
    unsigned claim=service_memory_wait(length <= M1_JSON_LIMIT ? length : 0);
    if(!claim){memory_busy(fd);return;}
    json_object *request=read_json(fd,length),*reply=service_command(request);
    if(request)json_object_put(request);
    send_json(fd,reply);service_memory_release(claim);
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
        custom_log("M1 HTTP", "Cannot listen on port 80"); mico_rtos_delete_thread(NULL); return;
    }
    int clients = HTTP_TCP_CLIENT_LIMIT;
    int set_rc = setsockopt(listener, IPPROTO_TCP, TCP_MAX_CONN_NUM, &clients, sizeof(clients));
    int set_errno = set_rc < 0 ? errno : 0;
    http_diag_sample(listener, set_rc, set_errno, true);
    if (set_rc < 0) custom_log("M1 HTTP", "Cannot raise TCP client limit");
    OSStatus err = mico_rtos_init_queue(&http_pending, "m1_http_pending", sizeof(int), HTTP_PENDING_CLIENTS);
    if (err != kNoErr) {
        close(listener);
        custom_log("M1 HTTP", "Cannot create pending client queue");
        mico_rtos_delete_thread(NULL); return;
    }
    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "m1_http", http_worker, 8192, 0);
    if (err != kNoErr) {
        mico_rtos_deinit_queue(&http_pending);
        close(listener);
        custom_log("M1 HTTP", "Cannot create request worker");
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
        /* Let the network owner/ACK timers progress under continuous overflow. */
        mico_thread_msleep(1);
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

OSStatus m1_http_start(void)
{ return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "m1_http_accept", http_accept_worker, 2048, 0); }
