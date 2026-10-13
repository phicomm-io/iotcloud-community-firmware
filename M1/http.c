#include "m1.h"
#include "web_data.h"
#include <ctype.h>
#include <errno.h>

typedef struct {
    char method[8], path[96], authorization[128];
    uint32_t length;
    bool have_length, chunked;
} request_t;

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

static bool response(int fd, unsigned code, const char *type, const void *body, size_t length, bool gzip)
{
    char header[320];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\n%s%s\r\n", code,
        code == 200 ? "OK" : code == 202 ? "Accepted" : code == 401 ? "Unauthorized" : "Error",
        type, (unsigned)length, gzip ? "Content-Encoding: gzip\r\n" : "",
        code == 401 ? "WWW-Authenticate: Basic realm=\"M1\"\r\n" : "");
    return m1_socket_send(fd, header, strlen(header)) && m1_socket_send(fd, body, length);
}

static void message(int fd, unsigned code, const char *text)
{
    json_object *o = json_object_new_object();
    if (!o) return;
    json_object_object_add(o, "message", json_object_new_string(text));
    const char *body = json_object_to_json_string(o);
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
            if (*end || length > M1_APP_OFFSET + M1_APP_LIMIT + 16) return false;
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
{ json_object_object_add(o, key, json_object_new_string(value)); }
static void integer(json_object *o, const char *key, int value)
{ json_object_object_add(o, key, json_object_new_int(value)); }
static void json_bool(json_object *o, const char *key, bool value)
{ json_object_object_add(o, key, json_object_new_boolean(value)); }

json_object *m1_status_json(void)
{
    m1_config_t cfg; m1_state_t s;
    m1_config_get(&cfg); m1_state_get(&s);
    uint32_t now = mico_rtos_get_time(); bool fresh = m1_sample_fresh(&s.sample, now);
    json_object *o = json_object_new_object();
    if (!o) return NULL;
    str(o, "version", M1_VERSION); str(o, "device_id", m1_id); str(o, "name", cfg.name);
    str(o, "ssid", cfg.ssid); str(o, "ip", s.ip); str(o, "ap_name", s.ap_name);
    str(o, "mqtt_host", cfg.mqtt_host); integer(o, "mqtt_port", cfg.mqtt_port); str(o, "mqtt_user", cfg.mqtt_user);
    integer(o, "timezone_minutes", cfg.timezone_minutes);
    json_bool(o, "web_password_set", cfg.web_password[0] != 0);
    json_bool(o, "wifi_connected", s.wifi_connected); json_bool(o, "mqtt_connected", s.mqtt_connected);
    json_bool(o, "ap_active", s.ap_active); json_bool(o, "uart_ready", s.uart_ready);
    json_bool(o, "sensor_valid", fresh); json_bool(o, "sensor_received", s.sample.received);
    json_object_object_add(o, "age_ms", s.sample.received ? json_object_new_double((uint32_t)(now - s.sample.updated_ms)) : NULL);
    integer(o, "brightness_requested", s.brightness_requested);
    json_object_object_add(o, "temperature", s.sample.received ? json_object_new_double(s.sample.temperature_deci_c / 10.0) : NULL);
    json_object_object_add(o, "humidity", s.sample.received ? json_object_new_double(s.sample.humidity_deci_pct / 10.0) : NULL);
    json_object_object_add(o, "formaldehyde", s.sample.received ? json_object_new_double(s.sample.hcho_ug_m3 / 1000.0) : NULL);
    json_object_object_add(o, "pm25", s.sample.received ? json_object_new_int(s.sample.pm25_ug_m3) : NULL);
    integer(o, "rx_frames", s.rx_frames); integer(o, "unknown_frames", s.unknown_frames);
    integer(o, "uart_errors", s.uart_errors); integer(o, "discarded_bytes", s.discarded_bytes);
    char frame[41];
    for (unsigned i = 0; i < 20; i++) snprintf(frame + i * 2, 3, "%02x", s.last_frame[i]);
    str(o, "last_frame", frame); str(o, "ota_state", s.ota_state); str(o, "ota_error", s.ota_error);
    integer(o, "ota_progress", s.ota_progress); json_bool(o, "ota_active", s.ota_active);
    json_bool(o, "scan_busy", s.scan_busy);
    return o;
}

static void send_json(int fd, json_object *o)
{
    if (!o) { message(fd, 500, "Out of memory"); return; }
    const char *body = json_object_to_json_string(o);
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
    json_object *o = received == length && !memchr(body, 0, received) ? json_tokener_parse(body) : NULL;
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
    if (strlen(value) >= capacity || strpbrk(value, "\r\n")) return false;
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
        json_object_array_add(list, ap);
    }
    json_object_object_add(o, "networks", list); send_json(fd, o);
}

static void handle(int fd)
{
    request_t req; m1_config_t cfg;
    if (!read_request(fd, &req)) { message(fd, 400, "Invalid HTTP request"); return; }
    m1_config_get(&cfg);
    if (!authorized(&req, cfg.web_password)) { message(fd, 401, "Authentication required"); return; }
    if (!strcmp(req.method, "GET")) {
        if (!strcmp(req.path, "/")) response(fd, 200, "text/html; charset=utf-8", m1_web_data, sizeof(m1_web_data), true);
        else if (!strcmp(req.path, "/wifi/scan")) scan_response(fd);
        else if (!strcmp(req.path, "/status") || !strcmp(req.path, "/wifi/config") ||
                 !strcmp(req.path, "/mqtt/config") || !strcmp(req.path, "/display/brightness") ||
                 !strcmp(req.path, "/ota")) send_json(fd, m1_status_json());
        else message(fd, 404, "Not found");
        return;
    }
    if (strcmp(req.method, "POST")) { message(fd, 405, "Use GET or POST"); return; }
    mico_rtos_lock_mutex(&m1_lock);
    bool busy = m1_state.ota_active || m1_state.reboot_pending;
    mico_rtos_unlock_mutex(&m1_lock);
    if (busy) { message(fd, 409, "OTA or reboot already in progress"); return; }
    if (!strcmp(req.path, "/ota/fileUpload")) {
        if (!req.have_length || !m1_ota_begin(req.length)) { message(fd, 400, "Invalid OTA size or busy"); return; }
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
    if (!strcmp(req.path, "/wifi/scan")) { message(fd, m1_scan_start() ? 202 : 409, "Scan requested"); return; }
    if (!strcmp(req.path, "/reboot")) { message(fd, 202, "Rebooting"); m1_reboot_later(1000); return; }
    json_object *o = read_json(fd, req.length);
    if (!o) { message(fd, 400, "Expected a bounded JSON object"); return; }
    bool valid = true, saved = false; int number;
    if (!strcmp(req.path, "/wifi/config")) {
        char old_ssid[33]; strcpy(old_ssid, cfg.ssid);
        valid = get_string(o, "ssid", cfg.ssid, sizeof(cfg.ssid), false) && cfg.ssid[0];
        if (strcmp(old_ssid, cfg.ssid)) cfg.wifi_password[0] = 0;
        valid = valid && get_string(o, "password", cfg.wifi_password, sizeof(cfg.wifi_password), !strcmp(old_ssid, cfg.ssid));
        json_object *open = json_object_object_get(o, "open_network");
        if (open && json_object_is_type(open, json_type_boolean) && json_object_get_boolean(open)) cfg.wifi_password[0] = 0;
        size_t n = strlen(cfg.wifi_password);
        valid = valid && (n == 0 || (n >= 8 && n <= 63));
        if (valid) saved = m1_config_save(&cfg);
    } else if (!strcmp(req.path, "/mqtt/config")) {
        valid = get_string(o, "host", cfg.mqtt_host, sizeof(cfg.mqtt_host), false) &&
            get_string(o, "user", cfg.mqtt_user, sizeof(cfg.mqtt_user), false) &&
            get_string(o, "password", cfg.mqtt_password, sizeof(cfg.mqtt_password), true) &&
            get_number(o, "port", &number, 1, 65535);
        json_object *clear = json_object_object_get(o, "clear_password");
        if (clear && json_object_is_type(clear, json_type_boolean) && json_object_get_boolean(clear))
            cfg.mqtt_password[0] = 0;
        if (valid) { cfg.mqtt_port = number; saved = m1_config_save(&cfg); }
    } else if (!strcmp(req.path, "/deviceName")) {
        valid = get_string(o, "name", cfg.name, sizeof(cfg.name), false) && cfg.name[0];
        if (valid) saved = m1_config_save(&cfg);
    } else if (!strcmp(req.path, "/auth/password")) {
        valid = get_string(o, "password", cfg.web_password, sizeof(cfg.web_password), false) &&
            json_object_object_get(o, "password");
        if (valid) saved = m1_config_save(&cfg);
    } else if (!strcmp(req.path, "/time/config")) {
        valid = get_number(o, "timezone_minutes", &number, -720, 840);
        if (valid) { cfg.timezone_minutes = number; saved = m1_config_save(&cfg); }
    } else if (!strcmp(req.path, "/display/brightness")) {
        valid = get_number(o, "level", &number, 0, 4);
        saved = valid && m1_set_brightness(number);
    } else if (!strcmp(req.path, "/ota")) {
        json_object *url = json_object_object_get(o, "url");
        valid = url && json_object_is_type(url, json_type_string);
        saved = valid && m1_ota_download(json_object_get_string(url));
    } else { message(fd, 404, "Not found"); json_object_put(o); return; }
    json_object_put(o);
    message(fd, !valid ? 400 : saved ? 202 : 503,
            !valid ? "Invalid parameters" : saved ? "Accepted" : "Unable to save or queue request");
}

static void http_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET;
    address.sin_port = htons(80); address.sin_addr.s_addr = INADDR_ANY;
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listener, 2) < 0) {
        if (listener >= 0) close(listener);
        custom_log("M1 HTTP", "Cannot listen on port 80"); mico_rtos_delete_thread(NULL); return;
    }
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd >= 0) { handle(fd); close(fd); }
        else mico_thread_msleep(100);
    }
}

OSStatus m1_http_start(void)
{ return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "m1_http", http_worker, 8192, 0); }
