#define _POSIX_C_SOURCE 200809L
#include "download.h"
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Host file models OTA staging only. No firmware activation is performed. */
typedef struct {
    const char *path;
    FILE *file;
    unsigned erases, writes, reads;
} staging;

static bool erase_stage(void *context)
{
    staging *s = context;
    s->erases++;
    s->file = fopen(s->path, "w+b");
    return s->file != NULL;
}
static bool write_stage(void *context, uint32_t offset, const uint8_t *data, size_t n)
{
    staging *s = context;
    s->writes++;
    return s->file && fseek(s->file, offset, SEEK_SET) == 0 &&
           fwrite(data, 1, n, s->file) == n;
}
static bool read_stage(void *context, uint32_t offset, uint8_t *data, size_t n)
{
    staging *s = context;
    s->reads++;
    return s->file && fseek(s->file, offset, SEEK_SET) == 0 &&
           fread(data, 1, n, s->file) == n;
}
static int read_socket(void *context, unsigned char *data, size_t n)
{
    ssize_t result;
    do { result = read(*(int *)context, data, n); } while (result < 0 && errno == EINTR);
    return result <= 0 ? -1 : (int)result;
}
static int write_socket(void *context, const unsigned char *data, size_t n)
{
    ssize_t result;
    do { result = write(*(int *)context, data, n); } while (result < 0 && errno == EINTR);
    return result <= 0 ? -1 : (int)result;
}
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int connect_peer(const char *host, const char *port)
{
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &addresses)) return -1;
    int fd = -1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval timeout = {15, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}
static int start_tls(probe_context *p, void *buffer, size_t capacity, const char *host, int *fd)
{
    unsigned char entropy[32];
    FILE *random = fopen("/dev/urandom", "rb");
    if (!random) return 0;
    size_t n = fread(entropy, 1, sizeof(entropy), random);
    fclose(random);
    if (n != sizeof(entropy)) return 0;
    probe_init(p, buffer, capacity);
    br_ssl_engine_inject_entropy(&p->client.eng, entropy, sizeof(entropy));
    if (!br_ssl_client_reset(&p->client, host, 0)) return 0;
    br_sslio_init(&p->io, &p->client.eng, read_socket, fd, write_socket, fd);
    return 1;
}
static int number(const char *text, unsigned long minimum, unsigned long maximum, unsigned long *result)
{
    char *end;
    errno = 0;
    *result = strtoul(text, &end, 10);
    return !errno && end != text && !*end && *result >= minimum && *result <= maximum;
}
typedef struct { const char *port; int fd; } transport;
static int open_transport(void *context, probe_context *p, void *buffer, size_t size, const char *host)
{
    transport *t = context;
    t->fd = connect_peer(host, t->port);
    if (t->fd < 0) return 0;
    if (!start_tls(p, buffer, size, host, &t->fd)) {
        close(t->fd);
        t->fd = -1;
        return 0;
    }
    return 1;
}
static void close_transport(void *context)
{
    transport *t = context;
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
}
static uint32_t millis(void *context)
{
    (void)context;
    return (uint32_t)((uint64_t)(now() * 1000));
}
int main(int argc, char **argv)
{
    if (argc != 7 && argc != 8) {
        fprintf(stderr, "usage: client HOST PATH BUFFER_BYTES RANGE_BYTES MODEL OUTPUT [PORT] (MODEL=0: metadata, max 16 KiB)\n");
        return 2;
    }
    unsigned long capacity, window, model;
    if (!number(argv[3], 837, BR_SSL_BUFSIZE_MONO, &capacity) ||
        !number(argv[4], 0, 8192, &window) || !number(argv[5], 0, 3, &model)) return 2;
    signal(SIGPIPE, SIG_IGN);
    probe_context *p = calloc(1, sizeof(*p));
    unsigned char *buffer = malloc(capacity);
    if (!p || !buffer) { free(p); free(buffer); return 2; }
    staging stage = {.path = argv[6]};
    transport t = {.port = argc == 8 ? argv[7] : "443", .fd = -1};
    probe_download_config config = {
        argv[1], argv[2], capacity, (uint32_t)window, (uint16_t)model,
        {erase_stage, write_stage, read_stage, &stage},
        &t, open_transport, close_transport, millis,
        model ? 0 : 16384, model ? NULL : write_stage, &stage
    };
    if (!model) {
        stage.file = fopen(stage.path, "w+b");
        if (!stage.file) { free(buffer); free(p); return 2; }
    }
    probe_download_result result;
    int ok = probe_download(&config, p, buffer, &result);
    if (stage.file && fclose(stage.file)) { result.error = "file_close"; ok = 0; }
    printf("ok=%d buffer=%lu context=%zu signed_context=%zu bytes=%"PRIu32" expected=%"PRIu32
           " requests=%"PRIu32" connections=%"PRIu32" erases=%u writes=%u reads=%u authenticated=%d"
           " tls_error=%d handshake_host_s=%.3f total_host_s=%.3f suite=0x%04x error=%s\n",
           ok, capacity, sizeof(*p), sizeof(result.session), result.bytes, result.total,
           result.requests, result.connections, stage.erases, stage.writes, stage.reads,
           result.session.authenticated, result.tls_error, result.first_request_ms / 1000.0,
           result.elapsed_ms / 1000.0, result.suite, result.error);
    free(buffer);
    free(p);
    return ok ? 0 : 1;
}
