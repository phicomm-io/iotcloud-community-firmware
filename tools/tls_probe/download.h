#ifndef PHICOMM_TLS_PROBE_DOWNLOAD_H
#define PHICOMM_TLS_PROBE_DOWNLOAD_H
#include "profile.h"
#include "signed_ota.h"
#define PROBE_MAX_IMAGE (1024u * 1024u)
#define PROBE_DEADLINE_MS 180000u

typedef struct {
    const char *host, *path;
    size_t buffer_size;
    uint32_t range_size;
    uint16_t model;
    signed_ota_io staging;
    void *transport;
    /* open initializes the TLS context and its synchronous I/O callbacks. */
    int (*open)(void *, probe_context *, void *, size_t, const char *);
    void (*close)(void *);
    uint32_t (*millis)(void *);
    /* model == 0: bounded metadata stream; never invokes OTA staging. */
    uint32_t stream_limit;
    bool (*stream_write)(void *, uint32_t, const uint8_t *, size_t);
    void *stream_context;
} probe_download_config;
typedef struct {
    signed_ota session;
    uint32_t bytes, total, requests, connections;
    uint32_t first_request_ms, elapsed_ms;
    int tls_error;
    uint16_t suite;
    const char *error;
} probe_download_result;
int probe_download(const probe_download_config *, probe_context *, void *, probe_download_result *);
#endif
