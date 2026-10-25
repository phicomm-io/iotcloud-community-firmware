/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hub_home_http.h"
#include "../../lib/HomeSpan/src/IoTCloudSRP.h"
#include <cassert>
#include <cstdio>
#include <random>

using iotcloud::HomeRequest;
static bool append(HomeRequest &r, const std::string &s) {
  return r.append(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}
static std::string post(const std::string &body) {
  return "POST /pair-setup HTTP/1.1\r\ncontent-type: application/pairing+tlv8\r\nCONTENT-LENGTH: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;
}
int main() {
  uint8_t nonce[12]={};
  for(unsigned i=0;i<65536;++i)assert(iotcloud::homeNonceIncrement(nonce));
  assert(nonce[4]==0&&nonce[5]==0&&nonce[6]==1);
  for(unsigned carry=4;carry<11;++carry){
    memset(nonce,0,sizeof(nonce));memset(nonce+4,0xff,carry-3);
    assert(iotcloud::homeNonceIncrement(nonce));
    for(unsigned i=0;i<=carry;++i)assert(nonce[i]==0);
    assert(nonce[carry+1]==1);
  }
  memset(nonce,0,sizeof(nonce));memset(nonce+4,0xff,8);
  assert(!iotcloud::homeNonceIncrement(nonce));
  for(auto byte:nonce)assert(!byte);
  const std::string binary("\x06\x01\x01\x00\xff\r\n\r\n", 9);
  const auto wire=post(binary);
  // Every possible TCP split, including a split within CRLF and binary NUL.
  for(size_t split=0;split<=wire.size();++split){
    HomeRequest r;
    assert(append(r,wire.substr(0,split)));
    assert(r.complete()==(split==wire.size()));
    assert(append(r,wire.substr(split)) && r.complete());
    assert(r.bytes==wire && r.type==HomeRequest::Pairing);
    r.clear();
    assert(!r.complete() && r.type==HomeRequest::Other && r.bytes.empty());
  }
  for(size_t step=1;step<wire.size();++step){
    HomeRequest r;
    for(size_t at=0;at<wire.size();at+=step)assert(append(r,wire.substr(at,step)));
    assert(r.complete());
  }
  HomeRequest get;
  assert(append(get,"GET /accessories HTTP/1.1\r\n\r\n") && get.complete());
  const std::string bad[]={
    "GET / HTTP/1.0\r\n\r\n", "GET / HTTP/1.1 junk\r\n\r\n",
    "GET http://host/ HTTP/1.1\r\n\r\n", "GET /has space HTTP/1.1\r\n\r\n",
    "GET / HTTP/1.1\r\nContent-Length: \r\n\r\n",
    "GET / HTTP/1.1\r\nContent-Length: +1\r\n\r\nx",
    "GET / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
    "GET / HTTP/1.1\r\nContent-Length: 1x\r\n\r\nx",
    "GET / HTTP/1.1\r\nContent-Length: 9999999999999999999999999999\r\n\r\n",
    "GET / HTTP/1.1\r\nContent-Length : 0\r\n\r\n",
    "GET / HTTP/1.1\r\nContent-Length: 0\r\ncontent-length: 0\r\n\r\n",
    "GET / HTTP/1.1\r\nContent-Type: a\r\nContent-Type: a\r\n\r\n",
    "GET / HTTP/1.1\r\nTransfer-Encoding: identity\r\n\r\n",
    "GET / HTTP/1.1\r\nX: folded\r\n continuation\r\n\r\n",
    "GET / HTTP/1.1\r\nNoColon\r\n\r\n",
    "GET / HTTP/1.1\r\nX: bare\nnewline\r\n\r\n",
    "GET / HTTP/1.1\r\n\r\nx",
    "GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n"
  };
  for(const auto &s:bad){HomeRequest r;assert(!append(r,s));}
  for(auto s:{std::string("GET / HTTP/1.1\r\nX: "),std::string("GET /")}){
    s.push_back(0);s+="bad HTTP/1.1\r\n\r\n";HomeRequest r;assert(!append(r,s));
  }
  HomeRequest oversized;
  assert(!append(oversized,std::string(HomeRequest::HeaderLimit+1,'a')));
  HomeRequest huge;
  assert(!append(huge,post(std::string(HomeRequest::Limit,'a'))));
  for(size_t n=1;n<=HomeRequest::Limit;++n){
    const auto s=post(std::string(n,'b'));
    HomeRequest r;
    assert(append(r,s)==(s.size()<=HomeRequest::Limit));
    if(s.size()==HomeRequest::Limit)assert(r.complete());
    if(s.size()>HomeRequest::Limit)break;
  }
  // Feed several coalesced encrypted records at arbitrary boundaries while
  // obeying the exact read size used by HAP.cpp. Ciphertext stays opaque here.
  std::vector<uint8_t> records;
  for(unsigned n:{1u,1024u,383u}){
    records.push_back(n&255);records.push_back(n>>8);
    records.insert(records.end(),n+16,0xa5);
  }
  for(size_t step=1;step<512;step+=7){
    iotcloud::HomeRecord record;size_t completed=0,at=0;
    while(at<records.size()){
      size_t n=std::min({step,record.remaining(),records.size()-at});
      assert(record.append(records.data()+at,n));at+=n;
      assert(record.bytes.size()<=1042);
      if(record.complete()){++completed;record.clear();}
    }
    assert(completed==3 && record.bytes.empty());
  }
  for(unsigned n:{0u,1025u,65535u}){
    iotcloud::HomeRecord record;uint8_t length[]={uint8_t(n),uint8_t(n>>8)};
    assert(!record.append(length,2));
  }
  iotcloud::HomeInputWindow window;
  window.opened(100);assert(!window.expired(3100,true));assert(window.expired(3101,true));
  assert(!window.expired(90000,false));
  window.received(1000);assert(window.expired(4001,false));
  for(uint32_t now=3000;now<=11000;now+=2000){window.received(now);assert(!window.expired(now,false));}
  window.received(11001);assert(window.expired(11001,false));
  window.completed();assert(!window.expired(20000,false));
  window.opened(0xfffffff0u);window.received(0xfffffff0u);
  assert(!window.expired(10,false));assert(window.expired(4000,false));
  const uint8_t tlv[]={6,1,1,0,1,0};
  assert(iotcloud::homeTlvValid(tlv,sizeof(tlv)));
  const uint8_t truncated[]={6,1,1,3,2,1};
  assert(!iotcloud::homeTlvValid(truncated,sizeof(truncated)));
  const uint8_t duplicate[]={6,1,1,6,1,1};
  assert(!iotcloud::homeTlvValid(duplicate,sizeof(duplicate)));
  std::vector<uint8_t> fragmented={3,255};fragmented.insert(fragmented.end(),255,0);
  fragmented.push_back(3);fragmented.push_back(129);fragmented.insert(fragmented.end(),129,0);
  assert(iotcloud::homeTlvValid(fragmented.data(),fragmented.size()));
  std::vector<uint8_t> flooded;
  for(unsigned i=0;i<17;++i){flooded.push_back(i);flooded.push_back(0);}
  assert(!iotcloud::homeTlvValid(flooded.data(),flooded.size()));
  // Public key and persisted verifier boundaries are checked before SRP MPI
  // import. Exercise a full-width 3072-bit modulus, leading padding, N and N+1.
  const std::string modulus=std::string(766,'F')+"43";
  std::vector<uint8_t> publicKey(384,0xff);publicKey.back()=0x43;
  assert(!iotcloud::homeSrpPublicKeyValid(publicKey.data(),publicKey.size(),modulus.c_str()));
  publicKey.back()=0x44;
  assert(!iotcloud::homeSrpPublicKeyValid(publicKey.data(),publicKey.size(),modulus.c_str()));
  publicKey.back()=0x42;
  assert(iotcloud::homeSrpPublicKeyValid(publicKey.data(),publicKey.size(),modulus.c_str()));
  std::fill(publicKey.begin(),publicKey.end(),0);
  assert(!iotcloud::homeSrpPublicKeyValid(publicKey.data(),publicKey.size(),modulus.c_str()));
  publicKey.back()=1;
  assert(iotcloud::homeSrpPublicKeyValid(publicKey.data(),publicKey.size(),modulus.c_str()));
  assert(iotcloud::homeSrpPublicKeyValid(publicKey.data()+383,1,modulus.c_str()));
  assert(!iotcloud::homeSrpPublicKeyValid(publicKey.data(),0,modulus.c_str()));
  publicKey.insert(publicKey.begin(),0);
  assert(!iotcloud::homeSrpPublicKeyValid(publicKey.data(),publicKey.size(),modulus.c_str()));
  // Fixed seed malformed input stresses parser indexes under sanitizers.
  std::mt19937 random(9231);
  for(unsigned i=0;i<4000;++i){
    std::string s(random()%2200,'\0');for(auto &c:s)c=random();
    HomeRequest r;append(r,s);
    iotcloud::homeTlvValid(reinterpret_cast<const uint8_t *>(s.data()),s.size());
  }
  puts("hub HAP HTTP/record/deadline/TLV/SRP-boundary/nonce tests passed");
}
