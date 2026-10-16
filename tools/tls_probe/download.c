#include "download.h"
#include <string.h>

#define LINE_SIZE 640u
typedef struct {
    uint32_t length, first, last, total;
    unsigned status, close, have_length, have_range;
} response;

static int read_line(probe_context *p, char *line, size_t capacity)
{
    size_t n = 0;
    while (n + 1 < capacity) {
        char ch;
        if (br_sslio_read_all(&p->io, &ch, 1) < 0 || ch == '\0') return -1;
        line[n++] = ch;
        if (ch == '\n') {
            if (n < 2 || line[n - 2] != '\r') return -1;
            line[n - 2] = 0;
            return (int)n;
        }
    }
    return -1;
}
static int prefix_ci(const char *text, const char *prefix)
{
    while (*prefix) {
        unsigned char ch = (unsigned char)*text++;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != (unsigned char)*prefix++) return 0;
    }
    return 1;
}
static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}
static int decimal(const char **position, uint32_t *value)
{
    const char *p = *position;
    uint32_t n = 0;
    if (*p < '0' || *p > '9') return 0;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (n > (UINT32_MAX - digit) / 10) return 0;
        n = n * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    *position = p;
    *value = n;
    return 1;
}
static int read_headers(probe_context *p, response *r, char line[LINE_SIZE])
{
    memset(r, 0, sizeof(*r));
    int n = read_line(p, line, LINE_SIZE);
    if (n < 0 || (strncmp(line, "HTTP/1.1 ", 9) && strncmp(line, "HTTP/1.0 ", 9))) return 0;
    const char *value = line + 9;
    uint32_t status;
    if (!decimal(&value, &status) || value != line + 12 || (*value && *value != ' ')) return 0;
    r->status = (unsigned)status;
    unsigned bytes = (unsigned)n;
    for (;;) {
        n = read_line(p, line, LINE_SIZE);
        if (n < 0 || (bytes += (unsigned)n) > 8192) return 0;
        if (!line[0]) break;
        if (prefix_ci(line, "content-length:")) {
            value = skip_space(line + 15);
            if (r->have_length++ || !decimal(&value, &r->length) || *skip_space(value) ||
                !r->length || r->length > PROBE_MAX_IMAGE + OTA_ENVELOPE_SIZE) return 0;
        } else if (prefix_ci(line, "content-range:")) {
            value = skip_space(line + 14);
            if (r->have_range++ || strncmp(value, "bytes ", 6)) return 0;
            value += 6;
            if (!decimal(&value, &r->first) || *value++ != '-' ||
                !decimal(&value, &r->last) || *value++ != '/' ||
                !decimal(&value, &r->total) || *skip_space(value)) return 0;
        } else if (prefix_ci(line, "transfer-encoding:") || prefix_ci(line, "content-encoding:")) {
            return 0;
        } else if (prefix_ci(line, "connection:")) {
            value = skip_space(line + 11);
            if (prefix_ci(value, "close") && !*skip_space(value + 5)) r->close = 1;
        }
    }
    return r->have_length == 1;
}
static int append(char *line, size_t *length, const char *text)
{
    size_t n = strlen(text);
    if (n >= LINE_SIZE - *length) return 0;
    memcpy(line + *length, text, n + 1);
    *length += n;
    return 1;
}
static int append_number(char *line, size_t *length, uint32_t value)
{
    char text[11];
    size_t n = sizeof(text) - 1;
    text[n] = 0;
    do { text[--n] = (char)('0' + value % 10); value /= 10; } while (value);
    return append(line, length, text + n);
}
static size_t request(char line[LINE_SIZE], const probe_download_config *c, uint32_t first, uint32_t last)
{
    size_t n = 0;
    if (!append(line, &n, "GET ") || !append(line, &n, c->path) ||
        !append(line, &n, " HTTP/1.1\r\nHost: ") || !append(line, &n, c->host)) return 0;
    if (c->range_size && (!append(line, &n, "\r\nRange: bytes=") ||
        !append_number(line, &n, first) || !append(line, &n, "-") || !append_number(line, &n, last))) return 0;
    if (!append(line, &n, c->range_size ? "\r\nConnection: keep-alive" : "\r\nConnection: close") ||
        !append(line, &n, "\r\nAccept-Encoding: identity\r\n\r\n")) return 0;
    return n;
}
int probe_download(const probe_download_config *c, probe_context *p, void *buffer, probe_download_result *out)
{
    char line[LINE_SIZE];
    unsigned char chunk[512];
    int connected = 0, ok = 1, started = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->error = "none";
    if (!c || !p || !buffer || !c->host || !c->path || !c->open || !c->close || !c->millis ||
        c->buffer_size < 837 || c->buffer_size > BR_SSL_BUFSIZE_MONO || c->range_size > 8192 ||
        c->model > 3 || (!c->model && (!c->stream_write || !c->stream_limit || c->stream_limit > 16384)) ||
        strlen(c->host) > 127 || strlen(c->path) > 383 ||
        c->path[0] != '/' || strpbrk(c->host, "\r\n ") || strpbrk(c->path, "\r\n ")) {
        out->error = "config";
        return 0;
    }
    uint32_t begin = c->millis(c->transport);
#define FAIL(reason) do { out->error = (reason); ok = 0; goto done; } while (0)
    while (!out->total || out->bytes < out->total) {
        if ((uint32_t)(c->millis(c->transport) - begin) > PROBE_DEADLINE_MS) FAIL("deadline");
        if (!connected) {
            if (!c->open(c->transport, p, buffer, c->buffer_size, c->host)) FAIL("connect");
            connected = 1;
            out->connections++;
        }
        uint32_t end = c->range_size ? out->bytes + c->range_size - 1 : 0;
        if (out->total && end >= out->total) end = out->total - 1;
        size_t length = request(line, c, out->bytes, end);
        if (!length || br_sslio_write_all(&p->io, line, length) < 0 || br_sslio_flush(&p->io) < 0) FAIL("request");
        if (!out->requests) out->first_request_ms = c->millis(c->transport) - begin;
        out->requests++;
        response r;
        if (!read_headers(p, &r, line)) FAIL("headers");
        if (c->range_size) {
            if (r.status != 206 || r.have_range != 1 || !r.total ||
                r.total > PROBE_MAX_IMAGE + OTA_ENVELOPE_SIZE || (out->total && r.total != out->total) ||
                r.first != out->bytes || r.last != (end < r.total ? end : r.total - 1) ||
                r.last < r.first || r.length != r.last - r.first + 1) FAIL("range");
        } else {
            if (r.status != 200 || r.have_range) FAIL("status");
            r.total = r.length;
        }
        if (!started) {
            out->total = r.total;
            if (!c->model) {
                if (out->total > c->stream_limit) FAIL("stream_limit");
            } else if (!signed_ota_begin(&out->session, out->total, PROBE_MAX_IMAGE, c->model, 1, c->staging)) FAIL("begin");
            started = 1;
        }
        uint32_t left = r.length;
        while (left) {
            if ((uint32_t)(c->millis(c->transport) - begin) > PROBE_DEADLINE_MS) FAIL("deadline");
            size_t want = left > sizeof(chunk) ? sizeof(chunk) : left;
            int n = br_sslio_read(&p->io, chunk, want);
            if (n <= 0) FAIL("body");
            if (!c->model) {
                if (!c->stream_write(c->stream_context, out->bytes, chunk, (size_t)n)) FAIL("stream_write");
            } else if (!signed_ota_write(&out->session, chunk, (size_t)n)) FAIL("signature_or_write");
            left -= (uint32_t)n;
            out->bytes += (uint32_t)n;
        }
        if (r.close) { c->close(c->transport); connected = 0; }
    }
    if (!started || (c->model && !signed_ota_finish(&out->session))) FAIL("readback");
done:
    out->elapsed_ms = c->millis(c->transport) - begin;
    if (out->connections) {
        out->tls_error = br_ssl_engine_last_error(&p->client.eng);
        out->suite = p->client.eng.session.cipher_suite;
    }
    if (connected) c->close(c->transport);
    return ok;
#undef FAIL
}
