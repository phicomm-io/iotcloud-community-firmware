/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hub_home_http.h"
#include "../../lib/HomeSpan/src/IoTCloudSRP.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>

// Model fragmentation of the actual C++ byte allocator after an earlier TCP
// fragment reserved enough input capacity. No server or parser is substituted.
static size_t allocationLimit = size_t(-1), allocationCalls = 0, allocationChecks = 0;
static bool countAllocations = false;
void *operator new(size_t n) {
  if (countAllocations) ++allocationCalls;
  if (n > allocationLimit) throw std::bad_alloc();
  if (auto p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { ::operator delete(p); }

using iotcloud::HomeRequest;
static std::vector<size_t> reservations;
static size_t allowedReservation;
static bool allocate(size_t n) {
  reservations.push_back(n);
  return n <= allowedReservation;
}
static bool append(HomeRequest &r, const std::string &s) {
  return r.append(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}
static std::string post(const std::string &body) {
  return "POST /pair-setup HTTP/1.1\r\ncontent-type: application/pairing+tlv8\r\nCONTENT-LENGTH: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;
}
static void headerAllocation() {
  const std::string prefix = "GET /accessories HTTP/1.1\r\n";
  const std::string headers[] = {
    std::string(900, 'x') + ": y\r\n\r\n",
    "Content-Type: " + std::string(900, 'X') + "\r\n\r\n",
    "cOnTeNt-TyPe: " + std::string(900, ' ') + "ApPlIcAtIoN/HaP+JsOn\t\r\n\r\n"
  };
  for (size_t i = 0; i < 3; ++i) {
    const auto wire = prefix + headers[i];
    if (!i) assert(wire.size() == 934);
    HomeRequest request;
    request.allocate = [](size_t n) { ++allocationChecks; return n < allocationLimit; };
    assert(request.append(reinterpret_cast<const uint8_t *>(wire.data()), 700));
    assert(request.append(reinterpret_cast<const uint8_t *>(wire.data() + 700), 100));
    assert(request.bytes.capacity() == 1400);
    const auto checked = allocationChecks;
    bool accepted = false, threw = false;
    allocationLimit = 600;
    allocationCalls = 0;
    countAllocations = true;
    try {
      accepted = request.append(reinterpret_cast<const uint8_t *>(wire.data() + 800), wire.size() - 800);
    } catch (const std::bad_alloc &) {
      threw = true;
    }
    countAllocations = false;
    allocationLimit = size_t(-1);
    assert(!threw && accepted && request.complete() && request.bytes == wire);
    assert(allocationCalls == 0 && allocationChecks == checked);
    assert(request.type == (i == 2 ? HomeRequest::Json : HomeRequest::Other));
  }
  for (const auto &mime : {std::string("ApPlIcAtIoN/PaIrInG+TlV8"), std::string("\tAPPLICATION/HAP+JSON \t")}) {
    HomeRequest request;
    assert(append(request, "POST /pair-setup HTTP/1.1\r\nCoNtEnT-TyPe: " + mime + "\r\nCoNtEnT-LeNgTh: 1\r\n\r\nx"));
    assert(request.complete() && request.type != HomeRequest::Other);
  }
}
static void requestAllocation() {
  auto wire = post(std::string(HomeRequest::Limit, 'b'));
  wire = post(std::string(HomeRequest::Limit - (wire.size() - HomeRequest::Limit), 'b'));
  assert(wire.size() == HomeRequest::Limit);
  HomeRequest request;
  request.allocate = allocate;
  allowedReservation = HomeRequest::Limit + 1;
  reservations.clear();
  assert(append(request, wire.substr(0, wire.size() - 1)));
  assert(request.bytes.capacity() == HomeRequest::Limit - 1);
  assert(reservations == std::vector<size_t>{HomeRequest::Limit});
  const auto before = request.bytes;
  allowedReservation = HomeRequest::Limit;
  assert(!append(request, wire.substr(wire.size() - 1)));
  assert(reservations.back() == HomeRequest::Limit + 1);
  assert(request.bytes == before && !request.complete());
  assert(request.bytes.capacity() == HomeRequest::Limit - 1);
  // The final byte checks 8097, then reserves only 8096. Appending directly to
  // the old 8095-byte capacity would grow this libstdc++ string to 16190.
  allowedReservation = HomeRequest::Limit + 1;
  assert(append(request, wire.substr(wire.size() - 1)) && request.complete());
  assert(request.bytes == wire && request.bytes.capacity() == HomeRequest::Limit);
  const auto checked = reservations.size();
  request.clear();
  allowedReservation = 0;
  assert(append(request, "GET /accessories HTTP/1.1\r\n\r\n") && request.complete());
  assert(reservations.size() == checked); // Reuse needs no new allocation.

  // Exercise growth through every TCP fragment size, preserving binary bodies.
  for (size_t step : {size_t(1), size_t(2), size_t(31), size_t(512), size_t(4096)}) {
    HomeRequest fragmented;
    fragmented.allocate = allocate;
    allowedReservation = HomeRequest::Limit + 1;
    reservations.clear();
    for (size_t at = 0; at < wire.size(); at += step) {
      assert(append(fragmented, wire.substr(at, step)));
      assert(fragmented.bytes.capacity() <= HomeRequest::Limit);
    }
    assert(fragmented.complete() && fragmented.bytes == wire);
    for (auto n : reservations) assert(n <= HomeRequest::Limit + 1);
  }
  HomeRequest denied;
  denied.allocate = allocate;
  allowedReservation = 0;
  reservations.clear();
  assert(!append(denied, "GET /accessories HTTP/1.1\r\n\r\n"));
  assert(denied.bytes.empty() && !denied.complete() && !denied.expected);
  assert(denied.type == HomeRequest::Other && reservations == std::vector<size_t>{33});
}
static void recordAllocation() {
  using iotcloud::HomeRecord;
  HomeRecord record;
  record.allocate = allocate;
  std::vector<uint8_t> input(HomeRecord::Limit + HomeRecord::Overhead, 0xa5);
  input[0] = 0; input[1] = 4;
  allowedReservation = HomeRecord::Limit + HomeRecord::Overhead;
  reservations.clear();
  assert(record.append(input.data(), 2));
  assert(reservations == std::vector<size_t>{32});
  assert(record.append(input.data() + 2, 1022));
  assert(record.bytes.capacity() == 1024 && reservations.back() == 1024);
  const auto before = record.bytes;
  allowedReservation = 1041;
  assert(!record.append(input.data() + 1024, 17));
  assert(reservations.back() == 1042 && record.bytes == before && !record.complete());
  allowedReservation = 1042;
  assert(record.append(input.data() + 1024, 17));
  assert(record.bytes.capacity() == 1042);
  const auto checked = reservations.size();
  allowedReservation = 0;
  assert(record.append(input.data() + 1041, 1) && record.complete());
  assert(reservations.size() == checked && record.bytes == input);
  record.clear();
  assert(record.append(input.data(), 2) && record.append(input.data() + 2, 1040));
  assert(record.complete() && reservations.size() == checked);

  // A one-byte final fragment must preflight the full replacement, not 1.
  HomeRecord finalByte;
  finalByte.allocate = allocate;
  allowedReservation = 1042;
  reservations.clear();
  assert(finalByte.append(input.data(), 2));
  assert(finalByte.append(input.data() + 2, 1039));
  assert(finalByte.bytes.capacity() == 1041);
  allowedReservation = 1041;
  assert(!finalByte.append(input.data() + 1041, 1));
  assert(reservations.back() == 1042 && finalByte.bytes.size() == 1041 && !finalByte.complete());
  allowedReservation = 1042;
  assert(finalByte.append(input.data() + 1041, 1) && finalByte.complete());
  assert(finalByte.bytes.capacity() == 1042 && finalByte.bytes == input);

  HomeRecord denied;
  denied.allocate = allocate;
  allowedReservation = 0;
  reservations.clear();
  assert(!denied.append(input.data(), 2));
  assert(denied.bytes.empty() && !denied.complete() && denied.remaining() == 2);
  assert(reservations == std::vector<size_t>{32});
}
int main() {
  headerAllocation();
  requestAllocation();
  recordAllocation();
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
