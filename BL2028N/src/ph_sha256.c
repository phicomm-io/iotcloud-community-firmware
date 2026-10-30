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

#endif
