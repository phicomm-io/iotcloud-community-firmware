/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../wire_format.h"
#ifndef PH_HTTP_CLIENT_SLOTS
#define PH_HTTP_CLIENT_SLOTS 4
#endif
#ifndef PH_HTTP_WAIT_SLOTS
#define PH_HTTP_WAIT_SLOTS 2
#endif
#if PH_HTTP_WAIT_SLOTS < 1 || PH_HTTP_WAIT_SLOTS >= PH_HTTP_CLIENT_SLOTS
#error HTTP waiting slots must fit behind the active connection
#endif
#define PH_HTTP_STACK_BYTES 12288u
#define PH_HTTP_HEADER_BYTES 4096u
#define PH_HTTP_LINE_BYTES 512u
#define PH_HTTP_HEADER_MS 5000u
#define PH_HTTP_READY_MS 1000u
#define PH_HTTP_RESPONSE_MS 10000u
#define PH_HTTP_FD_LIMIT 64

typedef struct {
    char method[8], path[96], authorization[128], origin[160], if_none_match[80];
    uint32_t length;
    /* Media flags: 0 absent, 1 recognized, 2 legacy/unknown. */
    unsigned have_length:1,have_type:2,have_accept:2;
    ph_wire_format content_format,accept_format;
} ph_http_request;
typedef struct {
    ph_http_request request;
    char line[PH_HTTP_LINE_BYTES];
    uint16_t used, bytes;
    uint8_t lines;
    bool cr, first, complete;
} ph_http_parser;
/* Consume precisely one byte. 1=complete header, 0=incomplete, -1=invalid. */
void ph_http_parser_init(ph_http_parser *);
int ph_http_parser_feed(ph_http_parser *, unsigned char);
bool ph_http_basic(const char *authorization, const char *user, const char *password);

typedef struct {
    unsigned status;
    const char *type, *cors, *realm, *etag;
    const void *body;
    size_t length;
    bool gzip, private_cache, suppress_body;
} ph_http_response;
typedef enum { PH_HTTP_REPLY, PH_HTTP_READ_BODY, PH_HTTP_WAIT } ph_http_action;
typedef struct {
    const ph_http_request *request;
    ph_http_response response;
    uint32_t body_idle_ms, body_total_ms;
} ph_http_exchange;
typedef struct {
    ph_http_action (*begin)(ph_http_exchange *);
    ph_http_action (*retry)(ph_http_exchange *);
    bool (*body)(ph_http_exchange *, const void *, size_t);
    void (*complete)(ph_http_exchange *);
    void (*release)(ph_http_exchange *, bool response_sent);
    unsigned (*admission)(void);
} ph_http_app;
typedef enum {
    PH_HTTP_CLOSED_OK, PH_HTTP_CLOSED_HEADER, PH_HTTP_CLOSED_TIMEOUT,
    PH_HTTP_CLOSED_PEER, PH_HTTP_CLOSED_IO, PH_HTTP_CLOSED_BUSY,
    PH_HTTP_CLOSED_STOP, PH_HTTP_CLOSE_REASONS
} ph_http_close_reason;
typedef struct {
    uint32_t accepted, rejected, accept_failures, nonblock_failures, drained, busy_sent, busy_failed, closed[PH_HTTP_CLOSE_REASONS];
    int last_fd, last_read_rc, last_select_rc, last_errno;
    int accept_errno, busy_errno, read_errno;
    uint32_t close_failures; int last_close_rc, last_close_errno;
    int set_rc, set_errno, limit_get_rc, limit, connections_get_rc, connections;
    uint32_t slot_bytes, server_bytes, queued, busy_bytes;
    uint32_t occupied, connections_sample_ms;
    unsigned owner_state, restarts; /* 0 stopped, 1 running, 2 unavailable, 3 uncertain close */
} ph_http_diagnostics;
/* OS abstraction: only the I/O owner calls these. retry() classifies the
 * immediately preceding error; cached diagnostics never perform socket I/O. */
typedef struct {
    uint32_t (*now)(void);
    int (*open)(unsigned cap);
    int (*limit)(int listener, unsigned cap, ph_http_diagnostics *);
    int (*accept)(int listener);
    int (*nonblock)(int fd);
    int (*wait)(uint64_t reads, uint64_t writes, uint64_t *ready_reads,
                uint64_t *ready_writes, unsigned timeout_ms);
    int (*recv)(int fd, void *, size_t);
    int (*send)(int fd, const void *, size_t);
    int (*close)(int fd);
    bool (*retry)(void);
    int (*error)(void);
    void (*yield)(void);
    void (*publish)(const ph_http_diagnostics *);
} ph_http_io;
typedef enum { PH_SLOT_FREE, PH_SLOT_HEADER, PH_SLOT_READY, PH_SLOT_WAIT,
               PH_SLOT_BODY, PH_SLOT_SEND, PH_SLOT_DRAIN } ph_http_state;
typedef struct {
    ph_http_parser parser;
    uint16_t in_used, in_pos;
    int fd;
    ph_http_state state;
    uint32_t deadline, idle_deadline, remaining;
    size_t header_sent, body_sent;
    const char *simple;
    bool lease;
    ph_http_close_reason reason;
} ph_http_slot;
typedef struct {
    ph_http_slot slots[PH_HTTP_CLIENT_SLOTS];
    ph_http_exchange exchange;
    const ph_http_io *io;
    const ph_http_app *app;
    ph_http_diagnostics diagnostics;
    char response_header[768];
    unsigned char scratch[512];
    size_t response_header_length;
    int listener, active, queued;
    unsigned cap, turn, accept_streak;
    bool running;
} ph_http_server;
bool ph_http_server_open(ph_http_server *, const ph_http_io *, const ph_http_app *, unsigned cap);
void ph_http_server_step(ph_http_server *);
void ph_http_server_close(ph_http_server *);
/* MiCO binding: starts one owner; stop is cooperative and never deletes a live
 * owner. Socket/core storage remains owned until that thread acknowledges. */
int ph_http_start(const ph_http_app *, unsigned cap);
int ph_http_stop(void);
ph_http_diagnostics ph_http_snapshot(void);
/* Stable MiCO owner handle, available only on the published HTTP thread. */
void *ph_http_json_owner(void);
