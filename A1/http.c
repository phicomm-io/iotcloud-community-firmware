#include "a1.h"
#include "../common/ota/signed_ota.h"
#include "web_data.h"
#include <ctype.h>
#include <errno.h>

typedef struct {
    char method[8], path[96], authorization[128];
    uint32_t length;
    bool have_length, chunked;
} request_t;

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

static bool response(int fd, unsigned code, const char *type, const void *body, size_t length, bool gzip)
{
    char header[320];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\n%s%s\r\n", code,
        code == 200 ? "OK" : code == 202 ? "Accepted" : code == 401 ? "Unauthorized" : "Error",
        type, (unsigned)length, gzip ? "Content-Encoding: gzip\r\n" : "",
        code == 401 ? "WWW-Authenticate: Basic realm=\"A1\"\r\n" : "");
    return a1_socket_send(fd, header, strlen(header)) && a1_socket_send(fd, body, length);
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
{ json_object_object_add(o, key, json_object_new_string(value)); }
static void integer(json_object *o, const char *key, int value)
{ json_object_object_add(o, key, json_object_new_int(value)); }
static void json_bool(json_object *o, const char *key, bool value)
{ json_object_object_add(o, key, json_object_new_boolean(value)); }

json_object *a1_status_json(void)
{
    a1_config_t cfg; a1_state_t s;
    a1_config_get(&cfg); a1_state_get(&s);
    json_object *o = json_object_new_object();
    if (!o) return NULL;
    str(o, "version", A1_VERSION); str(o, "device_id", a1_id); str(o, "name", cfg.name);
    str(o, "ssid", cfg.ssid); str(o, "ip", s.ip); str(o, "ap_name", s.ap_name);
    str(o, "mqtt_host", cfg.mqtt_host); integer(o, "mqtt_port", cfg.mqtt_port); str(o, "mqtt_user", cfg.mqtt_user);
    str(o, "mqtt_topic_mode", cfg.mqtt_topic_mode ? "account" : "legacy");
    str(o, "mqtt_status", mqtt_status_name(s.mqtt_status));
    json_bool(o, "mqtt_cleanup_warning", s.mqtt_cleanup_warning);
    json_bool(o, "web_password_set", cfg.web_password[0] != 0);
    json_bool(o, "wifi_password_set", cfg.wifi_password[0] != 0);
    json_bool(o, "mqtt_password_set", cfg.mqtt_password[0] != 0);
    json_bool(o, "wifi_connected", s.wifi_connected); json_bool(o, "mqtt_connected", s.mqtt_connected);
    bool effective_on; unsigned effective_speed; feature_effective_fan(cfg.fan_on,cfg.speed,&effective_on,&effective_speed);
    json_bool(o,"requested_fan_on",cfg.fan_on); integer(o,"requested_speed",cfg.speed);
    json_bool(o, "ap_active", s.ap_active); json_bool(o, "fan_on", effective_on);
    integer(o, "speed", cfg.speed); integer(o, "percentage", effective_on ? effective_speed : 0);
    integer(o, "applied_speed", s.applied_speed); json_bool(o, "fan_ready", s.fan_ready);
    json_bool(o, "led_enabled", cfg.led_enabled); json_bool(o, "led_visible", s.led_visible);
    json_bool(o, "led_always_on", cfg.led_always_on); integer(o, "brightness", cfg.brightness);
    integer(o, "red", cfg.red); integer(o, "green", cfg.green); integer(o, "blue", cfg.blue);
    json_bool(o, "child_lock", cfg.child_lock); json_bool(o, "panel_ready", s.panel_ready);
    integer(o, "panel_errors", s.panel_errors); integer(o, "hardware_errors", s.hardware_errors);
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
    const char *body = json_object_to_json_string(o);
    response(fd, 200, "application/json", body, strlen(body), false); json_object_put(o);
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
    json_object *o = received == length && !memchr(body, 0, received) ? a1_json_parse(body, received) : NULL;
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
        json_object_array_add(list, ap);
    }
    json_object_object_add(o, "networks", list); send_json(fd, o);
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

static void handle(int fd)
{
    request_t req; a1_config_t cfg;
    if (!read_request(fd, &req)) { message(fd, 400, "Invalid HTTP request"); return; }
    a1_config_get(&cfg);
    if (!authorized(&req, cfg.web_password)) { message(fd, 401, "Authentication required"); return; }
    const char *domain = feature_domain(req.path);
    if (!strcmp(req.method, "GET")) {
        if (domain) { send_json(fd, feature_json(domain)); return; }
        if (!strcmp(req.path, "/")) response(fd, 200, "text/html; charset=utf-8", a1_web_data, sizeof(a1_web_data), true);
        else if (!strcmp(req.path, "/wifi/scan")) scan_response(fd);
        else if (!strcmp(req.path, "/status") || !strcmp(req.path, "/wifi/config") ||
                 !strcmp(req.path, "/mqtt/config") || !strcmp(req.path, "/fan/config") || !strcmp(req.path, "/light/config") || !strcmp(req.path, "/childLock") ||
                 !strcmp(req.path, "/ota")) send_json(fd, a1_status_json());
        else message(fd, 404, "Not found");
        return;
    }
    if (strcmp(req.method, "POST")) { message(fd, 405, "Use GET or POST"); return; }
    mico_rtos_lock_mutex(&a1_lock);
    bool busy = a1_state.ota_active || a1_state.reboot_pending;
    mico_rtos_unlock_mutex(&a1_lock);
    if (busy) { message(fd, 409, "OTA or reboot already in progress"); return; }
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
    if (!strcmp(req.path, "/wifi/scan")) { message(fd, a1_scan_start() ? 202 : 409, "Scan requested"); return; }
    if (!strcmp(req.path, "/reboot")) { bool ok = a1_reboot_later(1000); message(fd, ok ? 202 : 503, ok ? "Rebooting" : "Config save failed or busy"); return; }
    json_object *o = read_json(fd, req.length);
    if (!o) { message(fd, 400, "Expected a bounded JSON object"); return; }
    if (domain) {
        json_object *result = NULL; int code = feature_set(domain, o, &result);
        json_object_put(o);
        if (!result) { message(fd, 500, "Out of memory"); return; }
        const char *body = json_object_to_json_string(result);
        response(fd, code, "application/json", body, strlen(body), false);
        json_object_put(result); return;
    }
    bool valid = true, saved = false; int number = 0;
    if (!strcmp(req.path, "/wifi/config")) {
        char old_ssid[33]; strcpy(old_ssid, cfg.ssid);
        valid = get_string(o, "ssid", cfg.ssid, sizeof(cfg.ssid), false) && cfg.ssid[0];
        if (strcmp(old_ssid, cfg.ssid)) cfg.wifi_password[0] = 0;
        valid = valid && get_string(o, "password", cfg.wifi_password, sizeof(cfg.wifi_password), !strcmp(old_ssid, cfg.ssid));
        json_object *open = json_object_object_get(o, "open_network");
        if (open && json_object_is_type(open, json_type_boolean) && json_object_get_boolean(open)) cfg.wifi_password[0] = 0;
        size_t n = strlen(cfg.wifi_password);
        valid = valid && (n == 0 || (n >= 8 && n <= 63));
        if (valid) saved = a1_config_save(&cfg);
        if (saved) { mico_rtos_lock_mutex(&a1_lock); a1_state.ap_requested = false; mico_rtos_unlock_mutex(&a1_lock); }
    } else if (!strcmp(req.path, "/mqtt/config")) {
        valid = get_string(o, "host", cfg.mqtt_host, sizeof(cfg.mqtt_host), false) &&
            get_string(o, "user", cfg.mqtt_user, sizeof(cfg.mqtt_user), false) &&
            get_string(o, "password", cfg.mqtt_password, sizeof(cfg.mqtt_password), true) &&
            get_number(o, "port", &number, 1, 65535);
        json_object *clear = json_object_object_get(o, "clear_password");
        if (clear && json_object_is_type(clear, json_type_boolean) && json_object_get_boolean(clear))
            cfg.mqtt_password[0] = 0;
        json_object *mode = json_object_object_get(o, "topic_mode");
        if (mode) {
            if (!json_object_is_type(mode, json_type_string) ||
                json_object_get_string_len(mode) != (int)strlen(json_object_get_string(mode)) ||
                (strcmp(json_object_get_string(mode), "legacy") && strcmp(json_object_get_string(mode), "account"))) valid = false;
            else cfg.mqtt_topic_mode = !strcmp(json_object_get_string(mode), "account");
        }
        if (cfg.mqtt_topic_mode && !cfg.mqtt_user[0]) valid = false;
        if (valid) { cfg.mqtt_port = number; saved = a1_config_save(&cfg); }
    } else if (!strcmp(req.path, "/deviceName")) {
        valid = get_string(o, "name", cfg.name, sizeof(cfg.name), false) && cfg.name[0];
        if (valid) saved = a1_config_save(&cfg);
    } else if (!strcmp(req.path, "/auth/password")) {
        valid = get_string(o, "password", cfg.web_password, sizeof(cfg.web_password), false) &&
            json_object_object_get(o, "password");
        if (valid) saved = a1_config_save(&cfg);
    } else if (!strcmp(req.path, "/fan/config") || !strcmp(req.path, "/light/config") || !strcmp(req.path, "/childLock")) {
        const char *kind = !strcmp(req.path, "/fan/config") ? "fan" : !strcmp(req.path, "/light/config") ? "light" : "child_lock";
        valid = a1_control_json(kind, o); saved = valid;
    } else if (!strcmp(req.path, "/ota")) {
        json_object *url = json_object_object_get(o, "url");
        valid = url && json_object_is_type(url, json_type_string);
        saved = valid && a1_ota_download(json_object_get_string(url));
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
        custom_log("A1 HTTP", "Cannot listen on port 80"); mico_rtos_delete_thread(NULL); return;
    }
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd >= 0) { handle(fd); close(fd); }
        else mico_thread_msleep(100);
    }
}

OSStatus a1_http_start(void)
{ return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_http", http_worker, 8192, 0); }
