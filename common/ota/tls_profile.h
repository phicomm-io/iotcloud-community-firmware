#ifndef PHICOMM_TLS_PROFILE_H
#define PHICOMM_TLS_PROFILE_H
/* Minimal TLS 1.2 profile; firmware authenticity is enforced by signed OTA. */
#include <bearssl.h>
typedef struct {
    const br_x509_class *vtable;
    br_x509_decoder_context decoder;
    unsigned certificates;
} ota_tls_x509;
typedef struct {
    br_ssl_client_context client;
    ota_tls_x509 x509;
    br_sslio_context io;
} ota_tls_context;
void ota_tls_init(ota_tls_context *p, void *buffer, size_t length);

#endif
