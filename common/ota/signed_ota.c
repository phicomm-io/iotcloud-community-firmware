#include "signed_ota.h"
#include "keyring.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
typedef mbedtls_sha256_context SHA256Context;
static int SHA256Reset(SHA256Context *s){mbedtls_sha256_init(s);return mbedtls_sha256_starts_ret(s,0);}
static int SHA256Input(SHA256Context *s,const uint8_t *p,size_t n){return mbedtls_sha256_update_ret(s,p,n);}
static int SHA256Result(SHA256Context *s,uint8_t out[32]){return mbedtls_sha256_finish_ret(s,out);}
static void sha_release(SHA256Context *s){mbedtls_sha256_free(s);}
static bool ota_verify(const uint8_t *key,const uint8_t *digest,const uint8_t *signature){
    mbedtls_ecdsa_context ec;mbedtls_ecdsa_init(&ec);mbedtls_mpi r,s;mbedtls_mpi_init(&r);mbedtls_mpi_init(&s);
    uint8_t point[65];point[0]=4;memcpy(point+1,key,64);
    bool ok=!mbedtls_ecp_group_load(&ec.grp,MBEDTLS_ECP_DP_SECP256R1)&&
      !mbedtls_ecp_point_read_binary(&ec.grp,&ec.Q,point,sizeof(point))&&
      !mbedtls_mpi_read_binary(&r,signature,32)&&!mbedtls_mpi_read_binary(&s,signature+32,32)&&
      !mbedtls_ecdsa_verify(&ec.grp,digest,32,&ec.Q,&r,&s);
    mbedtls_mpi_free(&r);mbedtls_mpi_free(&s);mbedtls_ecdsa_free(&ec);return ok;
}
#else
#include "vendor/uECC.h"
#include "sha.h"
static void sha_release(SHA256Context *s){(void)s;}
static bool ota_verify(const uint8_t *key,const uint8_t *digest,const uint8_t *signature){return uECC_verify(key,digest,32,signature,uECC_secp256r1());}
#endif
static uint16_t read16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static uint32_t read32(const uint8_t *p) { return read16(p) | (uint32_t)read16(p+2)<<16; }
static bool reject(signed_ota *s) { s->failed = true; return false; }
bool signed_ota_begin(signed_ota *s, uint32_t total, uint32_t maximum,
                      uint16_t model, uint16_t hardware, signed_ota_io io)
{
    memset(s, 0, sizeof(*s)); s->total=total; s->maximum=maximum;
    s->model=model; s->hardware=hardware; s->io=io;
    if (!io.erase || !io.write || !io.read || total<=OTA_ENVELOPE_SIZE ||
        total-OTA_ENVELOPE_SIZE>maximum) return reject(s);
    return true;
}
static bool authenticate(signed_ota *s)
{
    const uint8_t *h=s->envelope; uint8_t digest[32]; SHA256Context sha;
    const uint8_t *key=NULL; unsigned i;
    if (memcmp(h,"PHOTA001",8) || read16(h+8)!=1 || read16(h+10)!=128 ||
        read16(h+16)!=s->model || read16(h+18)!=s->hardware ||
        read32(h+20)!=s->total-OTA_ENVELOPE_SIZE ||
        (h[116]!=1 && h[116]!=2)) return false;
    for (i=117;i<128;i++) if (h[i]) return false;
    /* Canonical, printable, zero-padded version field. */
    bool ended=false;
    for (i=64;i<96;i++) {
        if (!h[i]) ended=true;
        else if (ended || h[i]<33 || h[i]>126) return false;
    }
    if (!ended || !h[64]) return false;
    for (i=0;i<OTA_KEY_COUNT;i++) if (ota_keys[i].id==read32(h+12)) key=ota_keys[i].public_key;
    if (!key) return false;
    bool hashed=!(SHA256Reset(&sha) ||
        SHA256Input(&sha,(const uint8_t *)"PHICOMM-OTA-V1",sizeof("PHICOMM-OTA-V1")-1) ||
        SHA256Input(&sha,h,128) || SHA256Result(&sha,digest));
    sha_release(&sha);
    if (!hashed || !ota_verify(key,digest,h+128)) return false;
    s->image_length=read32(h+20);
    /* Signature validated before the first destructive Flash operation. */
    if (!s->io.erase(s->io.context)) return false;
    s->authenticated=true; return true;
}
bool signed_ota_write(signed_ota *s, const uint8_t *data, size_t n)
{
    if (s->failed || !data || !n || n>s->total-s->received) return reject(s);
    if (s->received<OTA_ENVELOPE_SIZE) {
        size_t take=OTA_ENVELOPE_SIZE-s->received; if (take>n) take=n;
        memcpy(s->envelope+s->received,data,take); s->received+=take; data+=take; n-=take;
        if (s->received==OTA_ENVELOPE_SIZE && !authenticate(s)) return reject(s);
    }
    if (n) {
        if (!s->authenticated || !s->io.write(s->io.context,s->written,data,n)) return reject(s);
        s->written+=n; s->received+=n;
    }
    return true;
}
bool signed_ota_finish(signed_ota *s)
{
    uint8_t buffer[512], digest[32]; SHA256Context sha; uint32_t offset=0;
    if (s->failed || !s->authenticated || s->received!=s->total ||
        s->written!=s->image_length) return reject(s);
    if(SHA256Reset(&sha)){sha_release(&sha);return reject(s);}
    while (offset<s->image_length) {
        size_t n=s->image_length-offset; if (n>sizeof(buffer)) n=sizeof(buffer);
        if (!s->io.read(s->io.context,offset,buffer,n) || SHA256Input(&sha,buffer,n)) {sha_release(&sha);return reject(s);}
        offset+=n;
    }
    bool hashed=SHA256Result(&sha,digest)==0;sha_release(&sha);
    if (!hashed || memcmp(digest,s->envelope+32,32)) return reject(s);
    return true;
}
