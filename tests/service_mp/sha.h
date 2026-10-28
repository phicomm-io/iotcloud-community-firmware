#pragma once
#include <openssl/sha.h>
typedef SHA256_CTX SHA256Context;
static inline int SHA256Reset(SHA256Context *c){return !SHA256_Init(c);}
static inline int SHA256Input(SHA256Context *c,const unsigned char *p,size_t n){return !SHA256_Update(c,p,n);}
static inline int SHA256Result(SHA256Context *c,unsigned char *out){return !SHA256_Final(out,c);}
