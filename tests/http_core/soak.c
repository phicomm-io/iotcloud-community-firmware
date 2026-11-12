/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Long-running deterministic fake-I/O soak for the production HTTP core. */
#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FD_MAX PH_HTTP_FD_LIMIT
#define RX_CAP 256
#define TX_CAP 2048
#define MAX_SECONDS 14400u
#define MAX_MIN_ROUNDS 10000000u

typedef struct {
    unsigned char rx[RX_CAP];
    size_t rx_size, rx_at;
    char tx[TX_CAP];
    size_t tx_size;
    unsigned recv_again, send_again, recv_limit, send_limit;
    unsigned recv_fail_after, send_fail_after;
    unsigned close_count;
    bool eof, live, fail_recv, fail_send;
} endpoint;

typedef struct {
    uint64_t rounds, accepted, closed, lease_acquired, lease_released;
    uint64_t response_complete, response_failed, healthy_response, final_recovery;
    uint64_t queued_response, body_response, wait_response;
    uint64_t busy_response, busy_send_failed;
    uint64_t body_completed, wait_retries, waiting_full_rounds;
    uint64_t scenario[7], scenario_success[7];
    unsigned max_occupied;
} totals;

static ph_http_server server;
static endpoint endpoints[FD_MAX];
static unsigned pending[FD_MAX], pending_len;
static uint64_t live_fds;
static uint32_t tick_ms, prng;
static int io_error;
static bool vary_chunks;
static unsigned wait_retries_left;
static unsigned char reply_body[] = "ok";
static totals count;

static uint32_t random_u32(void)
{
    prng = prng * 1664525u + 1013904223u;
    return prng;
}
static uint32_t io_now(void) { return tick_ms; }
static int io_open(unsigned cap)
{
    (void)cap;
    endpoints[0].live = true;
    live_fds |= UINT64_C(1);
    return 0;
}
static int io_limit(int listener, unsigned cap, ph_http_diagnostics *diag)
{
    assert(listener == 0);
    diag->limit = (int)cap;
    return 0;
}
static int io_accept(int listener)
{
    assert(listener == 0);
    if (!pending_len) { io_error = EAGAIN; return -1; }
    unsigned fd = pending[0];
    memmove(pending, pending + 1, (--pending_len) * sizeof(pending[0]));
    assert(fd && fd < FD_MAX && !endpoints[fd].live);
    endpoints[fd].live = true;
    live_fds |= UINT64_C(1) << fd;
    ++count.accepted;
    return (int)fd;
}
static int io_nonblock(int fd) { return fd >= 0 && fd < FD_MAX ? 0 : -1; }
static int io_wait(uint64_t reads, uint64_t writes, uint64_t *ready_reads,
                   uint64_t *ready_writes, unsigned timeout_ms)
{
    assert((reads & ~live_fds) == 0);
    assert((writes & ~live_fds) == 0);
    *ready_reads = *ready_writes = 0;
    if ((reads & 1u) && pending_len) *ready_reads |= UINT64_C(1);
    for (unsigned fd = 1; fd < FD_MAX; ++fd) {
        if (!(reads & (UINT64_C(1) << fd))) continue;
        endpoint *ep = &endpoints[fd];
        if (ep->rx_at < ep->rx_size || ep->eof)
            *ready_reads |= UINT64_C(1) << fd;
    }
    *ready_writes = writes;
    tick_ms += (*ready_reads || *ready_writes) ? 1u : timeout_ms;
    return (*ready_reads || *ready_writes) ? 1 : 0;
}
static int io_recv(int fd, void *dst, size_t capacity)
{
    endpoint *ep = &endpoints[fd];
    if (ep->recv_again) { --ep->recv_again; io_error = EAGAIN; return -1; }
    if (ep->fail_recv && ep->rx_at >= ep->recv_fail_after) {
        ep->fail_recv = false;
        io_error = ECONNRESET;
        return -1;
    }
    if (ep->rx_at == ep->rx_size) {
        if (ep->eof) return 0;
        io_error = EAGAIN;
        return -1;
    }
    size_t n = ep->rx_size - ep->rx_at;
    if (n > capacity) n = capacity;
    unsigned limit = ep->recv_limit;
    if (vary_chunks) limit = 1u + random_u32() % 31u;
    if (limit && n > limit) n = limit;
    memcpy(dst, ep->rx + ep->rx_at, n);
    ep->rx_at += n;
    return (int)n;
}
static int io_send(int fd, const void *src, size_t n)
{
    endpoint *ep = &endpoints[fd];
    if (ep->send_again) { --ep->send_again; io_error = EAGAIN; return -1; }
    if (ep->fail_send && ep->tx_size >= ep->send_fail_after) {
        ep->fail_send = false;
        io_error = EPIPE;
        return -1;
    }
    unsigned limit = ep->send_limit;
    if (vary_chunks) limit = 1u + random_u32() % 37u;
    if (limit && n > limit) n = limit;
    assert(ep->tx_size + n < sizeof(ep->tx));
    memcpy(ep->tx + ep->tx_size, src, n);
    ep->tx_size += n;
    ep->tx[ep->tx_size] = 0;
    return (int)n;
}
static int io_close(int fd)
{
    assert(fd >= 0 && fd < FD_MAX && endpoints[fd].live);
    endpoint *ep = &endpoints[fd];
    assert(ep->close_count == 0);
    ep->live = false;
    ++ep->close_count;
    live_fds &= ~(UINT64_C(1) << fd);
    if (fd != 0) ++count.closed;
    return 0;
}
static bool io_retry(void) { return io_error == EAGAIN || io_error == EINTR; }
static int io_errno(void) { return io_error; }
static void io_yield(void) { ++tick_ms; }

static ph_http_action app_begin(ph_http_exchange *exchange)
{
    ++count.lease_acquired;
    exchange->response.status = 200;
    exchange->response.type = "application/json";
    exchange->response.body = reply_body;
    exchange->response.length = sizeof(reply_body) - 1;
    if (!strcmp(exchange->request->path, "/body")) {
        exchange->body_idle_ms = 1000;
        exchange->body_total_ms = 2000;
        return PH_HTTP_READ_BODY;
    }
    if (!strcmp(exchange->request->path, "/defer")) {
        wait_retries_left = 32;
        return PH_HTTP_WAIT;
    }
    return PH_HTTP_REPLY;
}
static ph_http_action app_retry(ph_http_exchange *exchange)
{
    if (!strcmp(exchange->request->path, "/defer")) {
        ++count.wait_retries;
        if (wait_retries_left) {
            --wait_retries_left;
            return PH_HTTP_WAIT;
        }
        return PH_HTTP_REPLY;
    }
    return PH_HTTP_WAIT;
}
static bool app_body(ph_http_exchange *exchange, const void *data, size_t size)
{
    (void)exchange;
    (void)data;
    return size != 0;
}
static void app_complete(ph_http_exchange *exchange)
{
    if (!strcmp(exchange->request->path, "/body"))
        ++count.body_completed;
}
static void app_release(ph_http_exchange *exchange, bool sent)
{
    assert(exchange->request != NULL);
    ++count.lease_released;
    if (sent) {
        ++count.response_complete;
        if (!strcmp(exchange->request->path, "/healthy"))
            ++count.healthy_response;
        if (!strcmp(exchange->request->path, "/final"))
            ++count.final_recovery;
        if (!strcmp(exchange->request->path, "/queued"))
            ++count.queued_response;
        if (!strcmp(exchange->request->path, "/body"))
            ++count.body_response;
        if (!strcmp(exchange->request->path, "/defer"))
            ++count.wait_response;
    } else ++count.response_failed;
}
static unsigned app_admission(void) { return 4; }

static const ph_http_io fake_io = {
    io_now, io_open, io_limit, io_accept, io_nonblock, io_wait, io_recv,
    io_send, io_close, io_retry, io_errno, io_yield, NULL
};
static const ph_http_app fake_app = {
    app_begin, app_retry, app_body, app_complete, app_release, app_admission
};

static void verify_invariants(void)
{
    unsigned occupied = 0, leases = 0;
    for (unsigned i = 0; i < PH_HTTP_CLIENT_SLOTS; ++i) {
        const ph_http_slot *slot = &server.slots[i];
        if (slot->state == PH_SLOT_FREE) {
            assert(slot->fd == -1 && !slot->lease);
            continue;
        }
        ++occupied;
        assert(slot->fd > 0 && slot->fd < FD_MAX);
        assert(endpoints[slot->fd].live);
        if (slot->lease) ++leases;
    }
    assert(occupied <= server.cap && occupied <= PH_HTTP_CLIENT_SLOTS);
    assert(leases <= 1);
    assert((server.active < 0 && leases == 0) ||
           (server.active >= 0 && leases == 1 &&
            server.slots[server.active].lease));
    assert(server.queued < 0 ||
           server.slots[server.queued].state == PH_SLOT_READY);
    if (occupied > count.max_occupied) count.max_occupied = occupied;
}
static void step(void)
{
    ph_http_server_step(&server);
    verify_invariants();
}
static void advance_until_closed(unsigned fd)
{
    for (unsigned i = 0; endpoints[fd].live && i < 1000; ++i) step();
    assert(!endpoints[fd].live);
}
static unsigned next_fd(unsigned *cursor)
{
    unsigned fd = *cursor;
    *cursor = fd + 1;
    if (*cursor >= FD_MAX) *cursor = 1;
    assert(!endpoints[fd].live);
    memset(&endpoints[fd], 0, sizeof(endpoints[fd]));
    return fd;
}
static void queue_fd(unsigned fd)
{
    assert(pending_len < FD_MAX);
    pending[pending_len++] = fd;
    step();
    assert(endpoints[fd].live);
}
static void append_bytes(unsigned fd, const void *data, size_t size)
{
    endpoint *ep = &endpoints[fd];
    assert(ep->rx_size + size <= sizeof(ep->rx));
    memcpy(ep->rx + ep->rx_size, data, size);
    ep->rx_size += size;
}
static int slot_for_fd(unsigned fd)
{
    for (unsigned i = 0; i < PH_HTTP_CLIENT_SLOTS; ++i)
        if (server.slots[i].state != PH_SLOT_FREE && server.slots[i].fd == (int)fd)
            return (int)i;
    return -1;
}
static void run_waiting_full_cycle(unsigned *fd_cursor, uint32_t round_index)
{
    unsigned owner = next_fd(fd_cursor);
    unsigned queued = next_fd(fd_cursor);
    unsigned queued_second = next_fd(fd_cursor);
    unsigned busy = next_fd(fd_cursor);
    bool use_body = (round_index & 1u) == 0;
    static const char body_head[] =
        "POST /body HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
    static const char wait_request[] = "GET /defer HTTP/1.1\r\n\r\n";
    static const char queued_request[] = "GET /queued HTTP/1.1\r\n\r\n";
    static const char busy_request[] = "GET /busy HTTP/1.1\r\n\r\n";

    if (use_body) append_bytes(owner, body_head, sizeof(body_head) - 1);
    else append_bytes(owner, wait_request, sizeof(wait_request) - 1);
    queue_fd(owner);
    for (unsigned i = 0; i < 128 && endpoints[owner].live && server.active < 0; ++i)
        step();
    assert(server.active >= 0 && server.slots[server.active].fd == (int)owner);
    assert(server.slots[server.active].lease);
    assert(use_body ? server.slots[server.active].state == PH_SLOT_BODY :
                      server.slots[server.active].state == PH_SLOT_WAIT);

    append_bytes(queued, queued_request, sizeof(queued_request) - 1);
    queue_fd(queued);
    for (unsigned i = 0; i < 128 && endpoints[queued].live && server.queued < 0; ++i)
        step();
    assert(server.queued >= 0 && server.slots[server.queued].fd == (int)queued);
    assert(server.slots[server.queued].state == PH_SLOT_READY);

    /* Fill both waiting slots before asking the fourth socket to return busy.
     * Waiting requests must not acquire a business lease or replace the FIFO. */
    append_bytes(queued_second, queued_request, sizeof(queued_request) - 1);
    queue_fd(queued_second);
    int second_slot = -1;
    for (unsigned i = 0; i < 128 && endpoints[queued_second].live; ++i) {
        second_slot = slot_for_fd(queued_second);
        if (second_slot >= 0 && server.slots[second_slot].state == PH_SLOT_READY) break;
        step();
    }
    assert(second_slot >= 0 && server.slots[second_slot].state == PH_SLOT_READY);
    assert(server.slots[server.queued].fd == (int)queued && !server.slots[second_slot].lease);

    bool fail_busy_send = round_index % 11u == 0;
    endpoints[busy].send_again = fail_busy_send ? 0 : round_index % 2u;
    endpoints[busy].fail_send = fail_busy_send;
    endpoints[busy].send_fail_after = 1;
    append_bytes(busy, busy_request, sizeof(busy_request) - 1);
    queue_fd(busy);
    int busy_slot = -1;
    for (unsigned i = 0; i < 128 && endpoints[busy].live; ++i) {
        busy_slot = slot_for_fd(busy);
        if (busy_slot >= 0 && server.slots[busy_slot].state != PH_SLOT_HEADER)
            break;
        step();
    }
    busy_slot = slot_for_fd(busy);
    assert(busy_slot >= 0 && server.slots[busy_slot].state == PH_SLOT_SEND);
    assert(server.active >= 0 && server.queued >= 0);
    assert(server.slots[server.active].fd == (int)owner);
    assert(server.slots[server.queued].fd == (int)queued);
    for (unsigned i = 0; i < 1000 && endpoints[busy].live; ++i) step();
    assert(!endpoints[busy].live);
    if (fail_busy_send) {
        assert(endpoints[busy].tx_size > 0);
        ++count.busy_send_failed;
    } else {
        assert(strstr(endpoints[busy].tx, "HTTP/1.1 503 Service Unavailable\r\n"));
        ++count.busy_response;
    }
    ++count.waiting_full_rounds;
    assert(count.max_occupied >= 4);

    if (use_body) {
        static const char body[] = "hello";
        append_bytes(owner, body, sizeof(body) - 1);
    }
    for (unsigned i = 0; i < 3000 && (endpoints[owner].live || endpoints[queued].live || endpoints[queued_second].live); ++i)
        step();
    assert(!endpoints[owner].live && !endpoints[queued].live && !endpoints[queued_second].live);
    assert(strstr(endpoints[queued].tx, "HTTP/1.1 200 OK\r\n"));
    assert(strstr(endpoints[queued_second].tx, "HTTP/1.1 200 OK\r\n"));
    assert(server.active == -1 && server.queued == -1);
    for (unsigned i = 0; i < PH_HTTP_CLIENT_SLOTS; ++i)
        assert(server.slots[i].state == PH_SLOT_FREE && !server.slots[i].lease);
    assert(endpoints[owner].close_count == 1);
    assert(endpoints[queued].close_count == 1);
    assert(endpoints[queued_second].close_count == 1);
    assert(endpoints[busy].close_count == 1);
    assert(count.lease_acquired == count.lease_released);
}
static void run_round(unsigned *fd_cursor, uint32_t round_index)
{
    unsigned slow = next_fd(fd_cursor);
    unsigned healthy = next_fd(fd_cursor);
    unsigned extra = next_fd(fd_cursor);
    static const char partial[] = "GET /slow HTTP/1.1\r\nX-Hold:";
    static const char healthy_request[] = "GET /healthy HTTP/1.1\r\nX-Round: 1\r\n\r\n";
    static const char body_request[] =
        "POST /body HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    static const char send_error_request[] =
        "POST /body HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    static const char malformed_request[] =
        "POST /bad HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n";
    static const char partial_request[] = "GET /partial HTTP/1.1\r\nX: incomplete";

    ++count.rounds;
    vary_chunks = (round_index & 1u) != 0;
    endpoints[slow].recv_again = round_index % 4 == 0 ? 1 : 0;
    endpoints[slow].recv_limit = 1u + random_u32() % 9u;
    queue_fd(slow);
    append_bytes(slow, partial, sizeof(partial) - 1);
    for (unsigned i = 0; i < 8; ++i) step();

    endpoints[healthy].recv_again = round_index % 5 == 0 ? 1 : 0;
    endpoints[healthy].send_again = round_index % 3 == 0 ? 1 : 0;
    queue_fd(healthy);
    append_bytes(healthy, healthy_request, sizeof(healthy_request) - 1);
    for (unsigned i = 0; i < 1000 && endpoints[healthy].live; ++i) step();
    assert(!endpoints[healthy].live);
    assert(strstr(endpoints[healthy].tx, "HTTP/1.1 200 OK\r\n"));
    assert(strstr(endpoints[healthy].tx, "Content-Length: 2\r\n"));
    assert(strstr(endpoints[healthy].tx, "\r\n\r\nok"));
    ++count.scenario[0];
    ++count.scenario_success[0];

    unsigned kind = random_u32() % 6u;
    if (kind == 0) {
        endpoints[extra].send_again = 1;
        queue_fd(extra);
        append_bytes(extra, body_request, sizeof(body_request) - 1);
        ++count.scenario[1];
        advance_until_closed(extra);
        assert(strstr(endpoints[extra].tx, "HTTP/1.1 200 OK\r\n"));
        ++count.scenario_success[1];
    } else if (kind == 1) {
        endpoints[extra].fail_send = true;
        endpoints[extra].send_fail_after = 1;
        queue_fd(extra);
        append_bytes(extra, send_error_request, sizeof(send_error_request) - 1);
        ++count.scenario[2];
        advance_until_closed(extra);
        assert(endpoints[extra].tx_size > 0);
        ++count.scenario_success[2];
    } else if (kind == 2) {
        queue_fd(extra);
        append_bytes(extra, malformed_request, sizeof(malformed_request) - 1);
        ++count.scenario[3];
        advance_until_closed(extra);
        assert(strstr(endpoints[extra].tx, "HTTP/1.1 400 Bad Request\r\n"));
        ++count.scenario_success[3];
    } else if (kind == 3) {
        endpoints[extra].eof = true;
        queue_fd(extra);
        ++count.scenario[4];
        advance_until_closed(extra);
        assert(endpoints[extra].tx_size == 0);
        ++count.scenario_success[4];
    } else if (kind == 4) {
        queue_fd(extra);
        append_bytes(extra, partial_request, sizeof(partial_request) - 1);
        for (unsigned i = 0; i < 3; ++i) step();
        endpoints[extra].eof = true;
        ++count.scenario[5];
        advance_until_closed(extra);
        assert(endpoints[extra].tx_size == 0);
        ++count.scenario_success[5];
    } else {
        endpoints[extra].fail_recv = true;
        endpoints[extra].recv_fail_after = 8;
        queue_fd(extra);
        append_bytes(extra, partial_request, sizeof(partial_request) - 1);
        ++count.scenario[6];
        advance_until_closed(extra);
        assert(endpoints[extra].tx_size == 0);
        ++count.scenario_success[6];
    }

    /* Release the long-lived partial header only after the healthy client has
     * completed, then prove it cannot retain a slot or lease. */
    tick_ms += PH_HTTP_HEADER_MS;
    step();
    assert(!endpoints[slow].live);
    assert(endpoints[slow].tx_size == 0);
    for (unsigned i = 0; i < PH_HTTP_CLIENT_SLOTS; ++i) {
        assert(server.slots[i].state == PH_SLOT_FREE);
        assert(server.slots[i].fd == -1 && !server.slots[i].lease);
    }
    assert(server.active == -1 && server.queued == -1);
    assert(count.lease_acquired == count.lease_released);
    assert(count.closed == count.accepted);
    if (round_index % 97u == 0)
        run_waiting_full_cycle(fd_cursor, round_index);
}

static double monotonic_seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}
static unsigned parse_unsigned(const char *text, unsigned base)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, base);
    if (errno || !text[0] || !end || *end || value > UINT32_MAX) {
        fprintf(stderr, "invalid unsigned option: %s\n", text);
        exit(2);
    }
    return (unsigned)value;
}
int main(int argc, char **argv)
{
    unsigned seconds = 2, min_rounds = 25000, seed = 0x51a7e, cursor = 1;
    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) { fprintf(stderr, "missing option value\n"); return 2; }
        if (!strcmp(argv[i], "--seconds")) seconds = parse_unsigned(argv[++i], 10);
        else if (!strcmp(argv[i], "--min-rounds")) min_rounds = parse_unsigned(argv[++i], 10);
        else if (!strcmp(argv[i], "--seed")) seed = parse_unsigned(argv[++i], 0);
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }
    if (!seconds || seconds > MAX_SECONDS || !min_rounds || min_rounds > MAX_MIN_ROUNDS) {
        fprintf(stderr, "seconds must be 1..%u and min-rounds 1..%u\n",
                MAX_SECONDS, MAX_MIN_ROUNDS);
        return 2;
    }
    prng = seed ? seed : 1u;
    memset(&server, 0, sizeof(server));
    memset(endpoints, 0, sizeof(endpoints));
    memset(&count, 0, sizeof(count));
    assert(ph_http_server_open(&server, &fake_io, &fake_app, 4));
    double started = monotonic_seconds();
    while (count.rounds < min_rounds || monotonic_seconds() - started < seconds) {
        run_round(&cursor, (uint32_t)count.rounds);
    }

    /* A final ordinary request must still work after the mixed soak. */
    unsigned final_fd = next_fd(&cursor);
    queue_fd(final_fd);
    static const char final_request[] = "GET /final HTTP/1.1\r\n\r\n";
    append_bytes(final_fd, final_request, sizeof(final_request) - 1);
    advance_until_closed(final_fd);
    assert(count.final_recovery == 1);
    assert(strstr(endpoints[final_fd].tx, "HTTP/1.1 200 OK\r\n"));
    assert(count.lease_acquired == count.lease_released);
    assert(count.response_complete + count.response_failed == count.lease_released);
    assert(count.response_failed > 0);
    assert(count.closed == count.accepted);
    assert(server.active == -1 && server.queued == -1);
    for (unsigned i = 0; i < PH_HTTP_CLIENT_SLOTS; ++i)
        assert(server.slots[i].state == PH_SLOT_FREE && !server.slots[i].lease);
    assert(count.healthy_response == count.rounds);
    for (unsigned i = 0; i < 7; ++i)
        assert(count.scenario[i] == count.scenario_success[i]);
    assert(count.waiting_full_rounds > 0 && count.busy_response > 0);
    assert(count.busy_send_failed > 0);
    assert(count.body_response > 0 && count.wait_response > 0);
    assert(count.queued_response == count.waiting_full_rounds * 2);
    assert(count.body_completed > 0 && count.wait_retries > 0);
    assert(server.diagnostics.slot_bytes == sizeof(ph_http_slot));
    assert(server.diagnostics.server_bytes == sizeof(ph_http_server));
    assert(count.max_occupied <= server.cap);
    for (unsigned i = 1; i < FD_MAX; ++i) assert(!endpoints[i].live);
    ph_http_server_close(&server);

    double elapsed = monotonic_seconds() - started;
    printf("shared HTTP soak PASS seconds=%u elapsed=%.3f rounds=%" PRIu64
           " seed=0x%08x accepted=%" PRIu64 " closed=%" PRIu64
           " leases=%" PRIu64 "/%" PRIu64 " responses=%" PRIu64
           " response_errors=%" PRIu64 " healthy_final=%" PRIu64
           " max_slots=%u\n",
           seconds, elapsed, count.rounds, seed, count.accepted, count.closed,
           count.lease_acquired, count.lease_released, count.response_complete,
           count.response_failed, count.final_recovery, count.max_occupied);
    printf("scenarios healthy=%" PRIu64 " body=%" PRIu64 " malformed=%" PRIu64
           " send_error=%" PRIu64 " eof=%" PRIu64 " partial_eof=%" PRIu64
           " recv_error=%" PRIu64 " waiting_full=%" PRIu64
           " body_lease=%" PRIu64 " wait_lease=%" PRIu64
           " queued_ok=%" PRIu64 " busy_503=%" PRIu64
           " busy_send_error=%" PRIu64
           " body_complete=%" PRIu64 " wait_retry=%" PRIu64
           " all_completed=yes\n",
           count.rounds, count.scenario[1], count.scenario[3],
           count.scenario[2], count.scenario[4], count.scenario[5],
           count.scenario[6], count.waiting_full_rounds, count.body_response,
           count.wait_response, count.queued_response, count.busy_response,
           count.busy_send_failed,
           count.body_completed, count.wait_retries);
    return 0;
}
