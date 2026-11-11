/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SHA256 only, from the pinned BK7231N SDK wolfCrypt source. This translation
 * unit selects its portable fixed-stack path and owns no TLS/RTOS state.
 * The build supplies the SDK wolfssl include root; host tests use that same
 * pinned source with an empty platform-config facade. */
#ifdef PH_S30_LAN_V4
#include "ph_sha256.h"
#define WOLFCRYPT_ONLY
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_ASN_TIME
#define NO_MD5
#define NO_MD4
#define NO_SHA
#define NO_PWDBASED
#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_ERROR_STRINGS
#define WOLFSSL_NO_ASM
#define NO_WOLFSSL_MEMORY
#define USE_SLOW_SHA256
#define NO_OLD_SHA_NAMES
#define NO_OLD_WC_NAMES
#include <wolfssl/wolfcrypt/settings.h>
#ifdef WOLFSSL_SMALL_STACK
#error SHA256 must use its fixed stack workspace
#endif
#define wc_InitSha256 ph_s30_InitSha256
#define wc_InitSha256_ex ph_s30_InitSha256_ex
#define wc_Sha256Update ph_s30_Sha256Update
#define wc_Sha256FinalRaw ph_s30_Sha256FinalRaw
#define wc_Sha256Final ph_s30_Sha256Final
#define wc_Sha256Free ph_s30_Sha256Free
#define wc_Sha256GetHash ph_s30_Sha256GetHash
#define wc_Sha256Copy ph_s30_Sha256Copy
#define wc_Sha256SetFlags ph_s30_Sha256SetFlags
#define wc_Sha256GetFlags ph_s30_Sha256GetFlags
#include <wolfcrypt/src/sha256.c>
bool PH_BL_Sha256(const void *bytes,size_t n,uint8_t out[32]) {
 if((!bytes&&n)||!out||n>UINT32_MAX)return false;
 wc_Sha256 context;int rc=wc_InitSha256(&context);
 if(!rc)rc=wc_Sha256Update(&context,bytes,(word32)n);
 if(!rc)rc=wc_Sha256Final(&context,out);
 wc_Sha256Free(&context);ForceZero(&context,sizeof(context));return !rc;
}

static wc_Sha256 ota_hash;
static uint32_t ota_size;
static uint8_t ota_digest[32];
bool PH_BL_OTAHashSet(const char *hex,uint32_t size) {
 ota_size=0;
 if(!hex)return true;
 for(unsigned i=0;i<32;i++) {
  unsigned value=0;
  for(unsigned j=0;j<2;j++) {
   unsigned char c=(unsigned char)hex[2*i+j];
   if(c>='0'&&c<='9')c-='0';
   else if(c>='a'&&c<='f')c=c-'a'+10;
   else if(c>='A'&&c<='F')c=c-'A'+10;
   else return false;
   value=(value<<4)|c;
  }
  ota_digest[i]=(uint8_t)value;
 }
 if(wc_InitSha256(&ota_hash))return false;
 ota_size=size;return true;
}
bool PH_BL_OTAHashUpdate(const void *bytes,size_t n,uint32_t total) {
 return !ota_size||(total==ota_size&&wc_Sha256Update(&ota_hash,bytes,(word32)n)==0);
}
bool PH_BL_OTAHashVerify(void) {
 if(!ota_size)return true;
 uint8_t digest[32];bool ok=wc_Sha256Final(&ota_hash,digest)==0&&!memcmp(digest,ota_digest,32);
 wc_Sha256Free(&ota_hash);ForceZero(&ota_hash,sizeof(ota_hash));ota_size=0;return ok;
}

#endif
