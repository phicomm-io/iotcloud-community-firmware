#ifndef PHICOMM_TLS_PROBE_PROFILE_H
#define PHICOMM_TLS_PROBE_PROFILE_H
/* Experimental unauthenticated TLS profile; not used by device firmware. */
#include <bearssl.h>
typedef struct {
    const br_x509_class *vtable;
    br_x509_decoder_context decoder;
    unsigned certificates;
} probe_x509;
typedef struct {
    br_ssl_client_context client;
    probe_x509 x509;
    br_sslio_context io;
} probe_context;
void probe_init(probe_context *p, void *buffer, size_t length);

#endif
