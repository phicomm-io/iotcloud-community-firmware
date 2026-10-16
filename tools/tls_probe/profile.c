#include "profile.h"
#include <string.h>

/* Decode the leaf public key only; publisher authentication is performed by
 * signed_ota.c. No CA, hostname, validity or certificate-signature validation. */
static void start_chain(const br_x509_class **context, const char *name)
{
    probe_x509 *x = (void *)context;
    (void)name;
    x->certificates = 0;
    br_x509_decoder_init(&x->decoder, NULL, NULL);
}
static void start_cert(const br_x509_class **context, uint32_t length)
{
    (void)context;
    (void)length;
}
static void append_cert(const br_x509_class **context, const unsigned char *data, size_t length)
{
    probe_x509 *x = (void *)context;
    if (!x->certificates) br_x509_decoder_push(&x->decoder, data, length);
}
static void end_cert(const br_x509_class **context)
{
    ((probe_x509 *)context)->certificates++;
}
static unsigned end_chain(const br_x509_class **context)
{
    br_x509_pkey *key = br_x509_decoder_get_pkey(&((probe_x509 *)context)->decoder);
    return key && key->key_type == BR_KEYTYPE_RSA ? 0 : BR_ERR_X509_INVALID_VALUE;
}
static const br_x509_pkey *public_key(const br_x509_class *const *context, unsigned *usages)
{
    if (usages) *usages = BR_KEYTYPE_SIGN | BR_KEYTYPE_KEYX;
    return br_x509_decoder_get_pkey(&((probe_x509 *)context)->decoder);
}
static const br_x509_class leaf_decoder = {
    sizeof(probe_x509), start_chain, start_cert, append_cert, end_cert, end_chain, public_key
};
void probe_init(probe_context *p, void *buffer, size_t length)
{
    static const uint16_t suites[] = {BR_TLS_RSA_WITH_AES_128_GCM_SHA256};
    memset(p, 0, sizeof(*p));
    p->x509.vtable = &leaf_decoder;
    br_ssl_client_zero(&p->client);
    br_ssl_engine_context *e = &p->client.eng;
    br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(e, suites, 1);
    br_ssl_engine_set_x509(e, &p->x509.vtable);
    br_ssl_client_set_rsapub(&p->client, br_rsa_i32_public);
    /* Also controls advertising RSA/SHA-256 in the signature_algorithms
     * extension. Omitting it makes modern OpenSSL servers reject the hello. */
    br_ssl_engine_set_rsavrfy(e, br_rsa_i32_pkcs1_vrfy);
    br_ssl_engine_set_hash(e, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_prf_sha256(e, &br_tls12_sha256_prf);
    br_ssl_engine_set_aes_ctr(e, &br_aes_small_ctr_vtable);
    br_ssl_engine_set_ghash(e, br_ghash_ctmul32);
    br_ssl_engine_set_gcm(e, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_add_flags(e, BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffer(e, buffer, length, 0);
}
