/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Adversarial host harness for the production parser and shared scheduler. */
#include "http.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FD_MAX 64
#define RX_MAX 8192
#define TX_MAX 16384

typedef struct {
    unsigned char rx[RX_MAX];
    size_t rx_len, rx_pos;
    char tx[TX_MAX];
    size_t tx_len;
    unsigned recv_chunk, send_chunk, recv_again, send_again, recv_count, close_count;
    bool eof, live;
} fake_fd;

static ph_http_server server;
static fake_fd fds[FD_MAX];
static unsigned tick, released, began, completed, body_calls, yield_count;
static uint32_t random_state;
static bool random_chunks;
static unsigned pending[FD_MAX], pending_count;
static uint64_t live_mask;
static int error_code;
static unsigned char response_body[256];

static uint32_t fake_now(void) { return tick; }
static int fake_open(unsigned cap) { (void)cap; fds[0].live = true; live_mask |= 1; return 0; }
static int fake_limit(int fd, unsigned cap, ph_http_diagnostics *d)
{ assert(fd == 0); d->limit = (int)cap; return 0; }
static int fake_accept(int fd)
{
    assert(fd == 0);
    if (!pending_count) { error_code = 11; return -1; }
    unsigned n = pending[0];
    memmove(pending, pending + 1, (--pending_count) * sizeof(pending[0]));
    fds[n].live = true;
    live_mask |= UINT64_C(1) << n;
    return (int)n;
}
static int fake_nonblock(int fd) { return fd >= 0 && fd < FD_MAX ? 0 : -1; }
static int fake_wait(uint64_t reads, uint64_t writes, uint64_t *rr, uint64_t *ww, unsigned ms)
{
    assert(!(reads & ~live_mask));
    assert(!(writes & ~live_mask));
    tick += ms;
    *rr = *ww = 0;
    if ((reads & 1) && pending_count) *rr |= 1;
    for (unsigned fd = 1; fd < FD_MAX; ++fd) {
        if (!(reads & (UINT64_C(1) << fd))) continue;
        if (fds[fd].rx_pos < fds[fd].rx_len || fds[fd].eof)
            *rr |= UINT64_C(1) << fd;
    }
    *ww = writes;
    return (*rr || *ww) ? 1 : 0;
}
static int fake_recv(int fd, void *dst, size_t cap)
{
    fake_fd *f = &fds[fd];
    ++f->recv_count;
    if (f->recv_again) { --f->recv_again; error_code = 11; return -1; }
    if (f->rx_pos == f->rx_len) {
        if (f->eof) return 0;
        error_code = 11;
        return -1;
    }
    size_t n = f->rx_len - f->rx_pos;
    if (n > cap) n = cap;
    unsigned chunk = f->recv_chunk;
    if (random_chunks) {
        random_state = random_state * 1664525u + 1013904223u;
        chunk = 1u + ((random_state >> 16) % 17u);
    }
    if (chunk && n > chunk) n = chunk;
    memcpy(dst, f->rx + f->rx_pos, n);
    f->rx_pos += n;
    return (int)n;
}
static int fake_send(int fd, const void *src, size_t n)
{
    fake_fd *f = &fds[fd];
    if (f->send_again) { --f->send_again; error_code = 11; return -1; }
    unsigned chunk = f->send_chunk;
    if (random_chunks) {
        random_state = random_state * 1664525u + 1013904223u;
        chunk = 1u + ((random_state >> 16) % 19u);
    }
    if (chunk && n > chunk) n = chunk;
    assert(f->tx_len + n < sizeof(f->tx));
    memcpy(f->tx + f->tx_len, src, n);
    f->tx_len += n;
    f->tx[f->tx_len] = 0;
    return (int)n;
}
static int fake_close(int fd)
{
    assert(fd >= 0 && fd < FD_MAX);
    ++fds[fd].close_count;
    fds[fd].live = false;
    live_mask &= ~(UINT64_C(1) << fd);
    return 0;
}
static bool fake_retry(void) { return error_code == 11 || error_code == 4; }
static int fake_error(void) { return error_code; }
static void fake_yield(void) { ++yield_count; ++tick; }

static ph_http_action begin(ph_http_exchange *x)
{
    ++began;
    x->response.status = 200;
    x->response.type = "application/json";
    x->response.body = response_body;
    x->response.length = sizeof(response_body);
    if (!strcmp(x->request->path, "/body")) {
        x->body_idle_ms = 250;
        x->body_total_ms = 750;
        return PH_HTTP_READ_BODY;
    }
    return PH_HTTP_REPLY;
}
static ph_http_action retry_exchange(ph_http_exchange *x)
{ (void)x; return PH_HTTP_WAIT; }
static bool body(ph_http_exchange *x, const void *p, size_t n)
{ (void)x; (void)p; body_calls += (unsigned)n; return true; }
static void complete(ph_http_exchange *x) { (void)x; ++completed; }
static void release(ph_http_exchange *x, bool sent)
{ assert(x->request != NULL); (void)sent; ++released; }
static unsigned admission(void) { return 3; }

static const ph_http_io io = {
    fake_now, fake_open, fake_limit, fake_accept, fake_nonblock, fake_wait,
    fake_recv, fake_send, fake_close, fake_retry, fake_error, fake_yield, NULL
};
static const ph_http_app app = { begin, retry_exchange, body, complete, release, admission };

static void reset(void)
{
    memset(&server, 0, sizeof(server));
    memset(fds, 0, sizeof(fds));
    memset(response_body, 'z', sizeof(response_body));
    tick = released = began = completed = body_calls = yield_count = 0;
    pending_count = 0;
    live_mask = 0;
    error_code = 0;
    random_state = 0x91e10da5u;
    random_chunks = false;
    assert(ph_http_server_open(&server, &io, &app, 3));
}
static void offer(unsigned fd)
{
    assert(fd > 0 && fd < FD_MAX && pending_count < FD_MAX);
    pending[pending_count++] = fd;
    ph_http_server_step(&server);
}
static void append(unsigned fd, const void *p, size_t n)
{
    fake_fd *f = &fds[fd];
    assert(f->rx_len + n <= sizeof(f->rx));
    memcpy(f->rx + f->rx_len, p, n);
    f->rx_len += n;
}
static void feed(unsigned fd, const void *p, size_t n, unsigned chunk)
{
    fds[fd].recv_chunk = chunk;
    append(fd, p, n);
    for (unsigned i = 0; i < n + 32 && server.running; ++i) ph_http_server_step(&server);
}
static int parse_bytes(const unsigned char *wire, size_t n)
{
    ph_http_parser p;
    ph_http_parser_init(&p);
    int rc = 0;
    for (size_t i = 0; i < n; ++i) {
        rc = ph_http_parser_feed(&p, wire[i]);
        if (rc) break;
    }
    return rc;
}
static size_t make_header_budget_vector(unsigned char *wire, size_t total)
{
    static const char request_line[] = "GET / HTTP/1.1\r\n";
    size_t used = sizeof(request_line) - 1;
    assert(total >= used + 2);
    memcpy(wire, request_line, used);
    size_t remaining = total - used - 2; /* reserve the final empty line */
    while (remaining) {
        size_t line_bytes = remaining > PH_HTTP_LINE_BYTES + 1u
            ? PH_HTTP_LINE_BYTES + 1u : remaining;
        assert(line_bytes >= 4);
        size_t content_bytes = line_bytes - 2;
        wire[used++] = 'X';
        wire[used++] = ':';
        memset(wire + used, 'a', content_bytes - 2);
        used += content_bytes - 2;
        wire[used++] = '\r';
        wire[used++] = '\n';
        remaining -= line_bytes;
    }
    wire[used++] = '\r';
    wire[used++] = '\n';
    assert(used == total);
    return used;
}
#define PARSE_LITERAL(s) parse_bytes((const unsigned char *)(s), sizeof(s) - 1)
#define APPEND_LITERAL(fd, s) append((fd), (s), sizeof(s) - 1)
#define FEED_LITERAL(fd, s, chunk) feed((fd), (s), sizeof(s) - 1, (chunk))

static void parser_vectors(void)
{
    assert(PARSE_LITERAL("GET /ok HTTP/1.1\r\nX-Tab:\tvalue\r\n\r\n") == 1);
    assert(PARSE_LITERAL("GET /ok HTTP/1.1\n\n") == -1);
    assert(PARSE_LITERAL("GET /ok HTTP/1.1\rX\n\r\n") == -1);
    assert(PARSE_LITERAL("GET /ok HTTP/1.2\r\n\r\n") == -1);
    assert(PARSE_LITERAL("GET /ok HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n") == -1);
    assert(PARSE_LITERAL("POST /ok HTTP/1.1\r\nContent-Length: 4294967296\r\n\r\n") == -1);
    assert(PARSE_LITERAL("POST /ok HTTP/1.1\r\nContent-Length: 4294967295\r\n\r\n") == 1);
    assert(PARSE_LITERAL("GET /ok HTTP/1.1\r\nContent-Length: 1x\r\n\r\n") == -1);
    assert(PARSE_LITERAL("GET /ok HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == -1);
    assert(PARSE_LITERAL("GET /ok HTTP/1.1\r\nExpect: 100-continue\r\n\r\n") == -1);
    const unsigned char nul_header[] = "GET /ok HTTP/1.1\r\nX: a\0b\r\n\r\n";
    assert(parse_bytes(nul_header, sizeof(nul_header) - 1) == -1);
    const unsigned char control_header[] = {
        'G','E','T',' ','/','o','k',' ','H','T','T','P','/','1','.','1','\r','\n',
        'X',':',' ','a',1,'b','\r','\n','\r','\n'
    };
    assert(parse_bytes(control_header, sizeof(control_header)) == -1);
    assert(PARSE_LITERAL("GET /bad path HTTP/1.1\r\n\r\n") == -1);
    assert(PARSE_LITERAL("get /ok HTTP/1.1\r\n\r\n") == -1);
    assert(!ph_http_basic("Basic YWRtaW46cHc=extra", "admin", "pw"));
    assert(!ph_http_basic("basic YWRtaW46cHc=", "admin", "pw"));

    char auth_wire[320];
    for (size_t value_len = 127; value_len <= 128; ++value_len) {
        int n = snprintf(auth_wire, sizeof(auth_wire), "GET / HTTP/1.1\r\nAuthorization: ");
        assert(n > 0);
        size_t used_auth = (size_t)n;
        memset(auth_wire + used_auth, 'a', value_len);
        used_auth += value_len;
        memcpy(auth_wire + used_auth, "\r\n\r\n", 4);
        used_auth += 4;
        assert(parse_bytes((const unsigned char *)auth_wire, used_auth) ==
               (value_len == 127 ? 1 : -1));
    }

    char wire[PH_HTTP_HEADER_BYTES + 32];
    size_t used = (size_t)snprintf(wire, sizeof(wire), "GET /ok HTTP/1.1\r\n");
    for (unsigned i = 0; i < 64; ++i) used += (size_t)snprintf(wire + used, sizeof(wire) - used, "X-%u: a\r\n", i);
    used += (size_t)snprintf(wire + used, sizeof(wire) - used, "\r\n");
    assert(parse_bytes((const unsigned char *)wire, used) == 1);
    used = (size_t)snprintf(wire, sizeof(wire), "GET /ok HTTP/1.1\r\n");
    for (unsigned i = 0; i < 65; ++i) used += (size_t)snprintf(wire + used, sizeof(wire) - used, "X-%u: a\r\n", i);
    used += (size_t)snprintf(wire + used, sizeof(wire) - used, "\r\n");
    assert(parse_bytes((const unsigned char *)wire, used) == -1);

    unsigned char budget[PH_HTTP_HEADER_BYTES + 1];
    size_t n = make_header_budget_vector(budget, PH_HTTP_HEADER_BYTES);
    assert(parse_bytes(budget, n) == 1);
    n = make_header_budget_vector(budget, PH_HTTP_HEADER_BYTES + 1u);
    assert(parse_bytes(budget, n) == -1);

    char path_request[256];
    char path[97];
    memset(path, 'p', sizeof(path));
    path[94] = 0;
    int path_len = snprintf(path_request, sizeof(path_request),
                            "GET /%s HTTP/1.1\r\n\r\n", path);
    assert(path_len > 0 && parse_bytes((const unsigned char *)path_request,
                                      (size_t)path_len) == 1);
    path[94] = 'p';
    path[95] = 0;
    path_len = snprintf(path_request, sizeof(path_request),
                        "GET /%s HTTP/1.1\r\n\r\n", path);
    assert(path_len > 0 && parse_bytes((const unsigned char *)path_request,
                                      (size_t)path_len) == -1);

    char line_request[700];
    size_t line_used = (size_t)snprintf(line_request, sizeof(line_request),
                                        "GET / HTTP/1.1\r\nX:");
    memset(line_request + line_used, 'v', 509);
    line_used += 509;
    memcpy(line_request + line_used, "\r\n\r\n", 4);
    line_used += 4;
    assert(parse_bytes((const unsigned char *)line_request, line_used) == 1);
    line_used = (size_t)snprintf(line_request, sizeof(line_request),
                                 "GET / HTTP/1.1\r\nX:");
    memset(line_request + line_used, 'v', 510);
    line_used += 510;
    memcpy(line_request + line_used, "\r\n\r\n", 4);
    line_used += 4;
    assert(parse_bytes((const unsigned char *)line_request, line_used) == -1);
}

static void fragmented_and_carried_body(void)
{
    reset();
    offer(1);
    static const char wire[] = "POST /body HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world";
    feed(1, wire, sizeof(wire) - 1, 0);
    assert(began == 1 && body_calls == 11 && completed == 1);
    assert(released == 1 && fds[1].close_count == 1);
    assert(fds[1].recv_count == 1); /* body bytes shared the header receive buffer */
    assert(strstr(fds[1].tx, "HTTP/1.1 200 OK\r\n"));
    assert(strstr(fds[1].tx, "Content-Length: 256\r\n"));
    assert(server.running);
    ph_http_server_close(&server);
    assert(fds[0].close_count == 1);
}

static void slow_header_does_not_starve_healthy_client(void)
{
    reset();
    offer(1);
    APPEND_LITERAL(1, "GET /slow HTTP/1.1\r\nX:");
    fds[1].recv_chunk = 2;
    for (unsigned i = 0; i < 12; ++i) ph_http_server_step(&server);
    assert(server.slots[0].state == PH_SLOT_HEADER);
    offer(2);
    FEED_LITERAL(2, "GET /healthy HTTP/1.1\r\n\r\n", 1);
    assert(began == 1 && strstr(fds[2].tx, "HTTP/1.1 200 OK\r\n"));
    assert(released == 1 && fds[2].close_count == 1);
    tick += PH_HTTP_HEADER_MS;
    ph_http_server_step(&server);
    assert(fds[1].close_count == 1);
    assert(released == 1);
    ph_http_server_close(&server);
}

static void transient_io_eof_and_wrap_deadline(void)
{
    reset();
    offer(1);
    random_chunks = true;
    fds[1].recv_again = 2;
    fds[1].send_again = 3;
    APPEND_LITERAL(1, "GET /partial HTTP/1.1\r\n\r\n");
    for (unsigned i = 0; i < 256; ++i) ph_http_server_step(&server);
    assert(strstr(fds[1].tx, "Content-Length: 256\r\n"));
    assert(released == 1 && fds[1].close_count == 1 && yield_count >= 5);
    assert(fds[1].recv_count > 1);
    ph_http_server_close(&server);

    reset();
    tick = UINT32_MAX - 1200u;
    offer(3);
    APPEND_LITERAL(3, "GET /slow HTTP/1.1\r\nX:");
    for (unsigned i = 0; i < 4; ++i) ph_http_server_step(&server);
    tick += PH_HTTP_HEADER_MS;
    ph_http_server_step(&server);
    assert(fds[3].close_count == 1 && released == 0);
    assert(server.running);
    ph_http_server_close(&server);
}

static void fragmented_fuzz_corpus(void)
{
    uint32_t state = 0x7f4a7c15u;
    for (unsigned trial = 0; trial < 64; ++trial) {
        char wire[384];
        size_t pos = 0;
        const char *prefix = trial & 1 ? "GET /fuzz HTTP/1.1\r\n" : "POST /body HTTP/1.1\r\nContent-Length: 0\r\n";
        size_t plen = strlen(prefix);
        memcpy(wire + pos, prefix, plen); pos += plen;
        unsigned headers = trial % 7;
        for (unsigned i = 0; i < headers; ++i) {
            state = state * 1664525u + 1013904223u;
            int n = snprintf(wire + pos, sizeof(wire) - pos, "X-%u: v%u\r\n", i, state & 255u);
            assert(n > 0 && (size_t)n < sizeof(wire) - pos);
            pos += (size_t)n;
        }
        memcpy(wire + pos, "\r\n", 2); pos += 2;
        if (trial % 5 == 0) wire[pos - 2] = '\n'; /* malformed LF-only terminator */
        int rc = parse_bytes((const unsigned char *)wire, pos);
        assert((trial % 5 == 0) ? rc == -1 : rc == 1);
    }
}

int main(void)
{
    parser_vectors();
    fragmented_and_carried_body();
    slow_header_does_not_starve_healthy_client();
    transient_io_eof_and_wrap_deadline();
    fragmented_fuzz_corpus();

    reset();
    offer(4);
    fds[4].eof = true;
    ph_http_server_step(&server);
    ph_http_server_step(&server);
    assert(fds[4].close_count == 1 && released == 0);
    ph_http_server_close(&server);
    assert(fds[0].close_count == 1);
    puts("shared HTTP adversarial parser/core tests PASS");
    return 0;
}
