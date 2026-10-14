/* Bounded direct HTTP transport. Authenticity comes from the signed package. */
#include "mico.h"
#include "ota_server.h"
#include "signed_adapter.h"
#include "user_ota.h"
#include "../../common/ota/signed_ota.h"
#include <ctype.h>
#include <string.h>
static ota_server_cb_fn callback;
int ota_socket_read(int fd, void *data, size_t n, unsigned timeout)
{
    fd_set readfds; FD_ZERO(&readfds); FD_SET(fd, &readfds);
    struct timeval tv = { timeout / 1000, (timeout % 1000) * 1000 };
    if (select(fd + 1, &readfds, NULL, NULL, &tv) <= 0) return -1;
    return recv(fd, data, n, 0);
}

bool ota_socket_send(int fd, const void *data, size_t n)
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

int ota_socket_line(int fd, char *line, size_t capacity)
{
    uint32_t deadline = mico_rtos_get_time() + 10000;
    size_t used = 0;
    while (used + 1 < capacity) {
        uint32_t now = mico_rtos_get_time();
        if ((int32_t)(now - deadline) >= 0) return -1;
        char c;
        if (ota_socket_read(fd, &c, 1, deadline - now) != 1 || !c) return -1;
        if (c == '\n') {
            if (!used || line[used - 1] != '\r') return -1;
            line[--used] = 0; return used;
        }
        line[used++] = c;
    }
    return -1;
}

static void download_worker(mico_thread_arg_t arg)
{
    char *url = (char *)arg;
    int fd = -1; const char *error = "HTTP download failed";
    char host[128], path[384], line[640]; unsigned port = 80;
    /* Direct HTTP URLs with Content-Length; no implicit redirect or TLS bypass. */
    const char *authority = url + 7, *slash = strchr(authority, '/');
    size_t authority_len = slash ? (size_t)(slash - authority) : strlen(authority);
    if (!authority_len || authority_len >= sizeof(host)) goto fail;
    memcpy(host, authority, authority_len); host[authority_len] = 0;
    if (slash && strlen(slash) >= sizeof(path)) { error = "URL path too long"; goto fail; }
    snprintf(path, sizeof(path), "%s", slash ? slash : "/");
    char *colon = strchr(host, ':');
    if (colon) {
        char *last; unsigned long value = strtoul(colon + 1, &last, 10);
        if (!value || value > 65535 || *last) goto fail;
        port = value; *colon = 0;
    }
    struct hostent *resolved = gethostbyname(host);
    if (!resolved || !resolved->h_addr_list[0]) { error = "DNS lookup failed"; goto fail; }
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_port = htons(port);
    memcpy(&address.sin_addr, resolved->h_addr_list[0], 4);
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) goto fail;
    snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n", path, host, port);
    if (!ota_socket_send(fd, line, strlen(line)) || ota_socket_line(fd, line, sizeof(line)) < 0 ||
        (strncmp(line, "HTTP/1.1 200 ", 13) && strncmp(line, "HTTP/1.0 200 ", 13))) {
        error = "Download requires a direct HTTP 200 response"; goto fail;
    }
    uint32_t length = 0; bool have_length = false; unsigned headers = 0;
    for (;;) {
        int n = ota_socket_line(fd, line, sizeof(line));
        if (n < 0 || (headers += n) > 4096) goto fail;
        if (!n) break;
        if (!strncasecmp(line, "Content-Length:", 15)) {
            char *end; unsigned long parsed = strtoul(line + 15, &end, 10);
            while (*end == ' ') end++;
            if (have_length || *end || parsed > 0xa9000u + OTA_ENVELOPE_SIZE) goto fail;
            length = parsed; have_length = true;
        }
        if (!strncasecmp(line, "Transfer-Encoding:", 18) ||
            (!strncasecmp(line, "Content-Encoding:", 17) && strcmp(line + 18, "identity"))) {
            error = "Chunked or compressed downloads are unsupported"; goto fail;
        }
    }
    if (!have_length) { error = "Download is missing Content-Length"; goto fail; }
    if(!tc1_ota_begin(length,false)) {error="Invalid signed package length";goto fail;}
    uint32_t received=0;
    uint8_t data[512]; uint32_t deadline = mico_rtos_get_time() + 300000;
    while (received < length) {
        if ((int32_t)(mico_rtos_get_time() - deadline) >= 0) { error = "Download timed out"; goto fail; }
        unsigned want = length - received > sizeof(data) ? sizeof(data) : length - received;
        int n = ota_socket_read(fd, data, want, 15000);
        if (n <= 0) { error = "Download interrupted"; goto fail; }
        if (!tc1_ota_write(data,n)) {error="Signature or Flash write failed";goto fail;}
        received+=n;
        ota_progress=(float)received*100/length;
        if(callback)callback(OTA_LOADING,ota_progress);
    }
    if(!tc1_ota_finish()) {error="OTA image or boot metadata validation failed";goto fail;}
    ota_progress=100;
    if(callback)callback(OTA_SUCCE,100);
    mico_thread_msleep(1500);
    mico_system_power_perform(mico_system_context_get(),eState_Software_Reset);
    goto done;
fail:
    ota_progress=-1;
    custom_log("OTA","%s",error);if(callback)callback(OTA_FAIL,0);
    tc1_ota_abort();
done:
    if (fd >= 0) close(fd);
    free(url); mico_rtos_delete_thread(NULL);
}

OSStatus ota_server_start(char *url,char *md5,ota_server_cb_fn cb)
{
    (void)md5; /* A broker-supplied MD5 is never an authorization credential. */
    if(!url || strncmp(url,"http://",7) || strlen(url)>=500 || strpbrk(url,"\r\n @#") || !tc1_ota_reserve())return kParamErr;
    char *copy=malloc(strlen(url)+1);
    if(!copy){tc1_ota_abort();return kNoMemoryErr;}strcpy(copy,url);callback=cb;ota_progress=0;
    OSStatus err=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"OTA",download_worker,6144,(mico_thread_arg_t)copy);
    if(err!=kNoErr){free(copy);tc1_ota_abort();}return err;
}
