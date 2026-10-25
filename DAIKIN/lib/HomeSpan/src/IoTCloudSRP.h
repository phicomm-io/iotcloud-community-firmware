/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace iotcloud {
// RFC 5054: reject the zero public value and values outside 0 < A < N.
// HomeSpan's group modulus is a full-width, big-endian hexadecimal constant.
// Check bytes before importing an attacker-controlled MPI or deriving K.
inline bool homeSrpPublicKeyValid(const uint8_t *key, size_t length,
                                 const char *modulusHex) {
  const auto width=std::strlen(modulusHex)/2;
  if(!key || !length || length>width)return false;
  bool nonzero=false;
  for(size_t i=0;i<length;++i)nonzero|=key[i]!=0;
  if(!nonzero)return false;
  if(length<width)return true;
  for(size_t i=0;i<width;++i){
    unsigned byte=0;
    for(unsigned n=0;n<2;++n){
      const auto c=modulusHex[i*2+n];
      byte=byte*16+(c>='0'&&c<='9'?c-'0':c-'A'+10);
    }
    if(key[i]<byte)return true;
    if(key[i]>byte)return false;
  }
  return false;
}
} // namespace iotcloud
