#include <Arduino.h>
#include "IoTCloudHapWriter.h"
#include "TLV8.h"
#include "hub_home_http.h"
#include <cassert>
#include <climits>
#include <limits>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sstream>

HostSerial Serial;
using boolean = bool;
static const unsigned MALLOC_CAP_DEFAULT=1, MALLOC_CAP_INTERNAL=2;
static void *heap_caps_malloc(size_t size, unsigned) {return malloc(size);}
static void delay(unsigned) {}
static struct {int getLogLevel() const {return 0;}} homeSpan;

using mbedtls_sha512_context = SHA512_CTX;
static void mbedtls_sha512_init(mbedtls_sha512_context *) {}
static void mbedtls_sha512_starts_ret(mbedtls_sha512_context *ctx, int sha384) {
  assert(sha384==1);assert(SHA384_Init(ctx)==1);
}
static void mbedtls_sha512_update_ret(mbedtls_sha512_context *ctx, const uint8_t *p, size_t n) {
  assert(SHA384_Update(ctx,p,n)==1);
}
static void mbedtls_sha512_finish_ret(mbedtls_sha512_context *ctx, uint8_t *hash) {
  assert(SHA384_Final(hash,ctx)==1);
}

static bool encryptFailure=false;
static int crypto_aead_chacha20poly1305_ietf_encrypt(uint8_t *cipher,
    unsigned long long *cipherSize, const uint8_t *plain, unsigned long long size,
    const uint8_t *aad, unsigned long long aadSize, const uint8_t *,
    const uint8_t *nonce, const uint8_t *key) {
  if(encryptFailure)return -1;
  auto ctx=EVP_CIPHER_CTX_new();assert(ctx);
  int n=0,tail=0;
  assert(EVP_EncryptInit_ex(ctx,EVP_chacha20_poly1305(),nullptr,key,nonce)==1);
  assert(EVP_EncryptUpdate(ctx,nullptr,&n,aad,aadSize)==1);
  assert(EVP_EncryptUpdate(ctx,cipher,&n,plain,size)==1 && size_t(n)==size);
  assert(EVP_EncryptFinal_ex(ctx,cipher+n,&tail)==1 && tail==0);
  assert(EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_GET_TAG,16,cipher+size)==1);
  if(cipherSize)*cipherSize=size+16;
  EVP_CIPHER_CTX_free(ctx);
  return 0;
}

#include "nonce-class.inc"
struct HostClient {
  std::vector<std::vector<uint8_t>> records;
  size_t maxWrite=SIZE_MAX;
  bool stopped=false;
  size_t write(const char *data, size_t size) {
    return write(reinterpret_cast<const uint8_t *>(data),size);
  }
  size_t write(const uint8_t *data, size_t size) {
    assert(!stopped);
    const auto written=std::min(maxWrite,size);
    records.emplace_back(data,data+written);
    return written;
  }
  void stop() {stopped=true;}
};
struct HAPClient {
  void *cPair=nullptr;
  HostClient client;
  Nonce a2cNonce;
  uint8_t a2cKey[32]={};
};
#include "hap-class.inc"
static_assert(!std::is_copy_constructible<HapOut>::value,"HAP buffers must not be copied");
static_assert(!std::is_copy_assignable<HapOut>::value,"HAP buffers must not be copied");
static_assert(!std::is_move_constructible<HapOut>::value,"HAP buffers must not be moved");
#include "hap-methods.inc"
#include "tlv-methods.inc"

struct Collect : iotcloud::HapByteWriter {
  std::string bytes;
  void write(const char *p,size_t n) override {bytes.append(p,n);}
};
static std::string payload(size_t n) {
  std::string s(n,'\0');
  for(size_t i=0;i<n;i++)s[i]=char((i*73)%256);
  return s;
}
static void hashEquals(HapOut &out,const std::string &plain) {
  uint8_t hash[48];
  assert(SHA384(reinterpret_cast<const uint8_t *>(plain.data()),plain.size(),hash));
  assert(!memcmp(out.getHash(),hash,sizeof hash));
}
static std::string decrypt(const HAPClient &hap,const std::vector<uint8_t> &record,
                           uint8_t (&nonce)[12]) {
  assert(record.size()>=19);
  const size_t size=record[0]+size_t(record[1])*256;
  assert(size>0 && size<=1024 && record.size()==size+18);
  std::string plain(size,'\0');
  auto ctx=EVP_CIPHER_CTX_new();assert(ctx);
  int n=0,tail=0;
  assert(EVP_DecryptInit_ex(ctx,EVP_chacha20_poly1305(),nullptr,hap.a2cKey,nonce)==1);
  assert(EVP_DecryptUpdate(ctx,nullptr,&n,record.data(),2)==1);
  assert(EVP_DecryptUpdate(ctx,reinterpret_cast<uint8_t *>(&plain[0]),&n,
                          record.data()+2,size)==1 && size_t(n)==size);
  assert(EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_SET_TAG,16,
                          const_cast<uint8_t *>(record.data()+2+size))==1);
  assert(EVP_DecryptFinal_ex(ctx,reinterpret_cast<uint8_t *>(&plain[0])+size,&tail)==1 && tail==0);
  EVP_CIPHER_CTX_free(ctx);
  iotcloud::homeNonceIncrement(nonce);
  return plain;
}

static void numbers() {
  Collect writer;
  std::ostringstream reference;
  const char *escaped="a\\\"b\\\\c\\n";
  writer << "{\"status\":" << -70410 << ",\"aid\":" << UINT64_MAX
         << ",\"min\":" << INT64_MIN << ",\"name\":\"" << escaped << "\"}";
  reference << "{\"status\":" << -70410 << ",\"aid\":" << UINT64_MAX
            << ",\"min\":" << INT64_MIN << ",\"name\":\"" << escaped << "\"}";
  assert(writer.bytes==reference.str());
  uint64_t state=1234567;
  for(unsigned i=0;i<10000;i++) {
    state=state*6364136223846793005ULL+1;
    writer.bytes.clear();reference.str("");
    writer << state << ':' << static_cast<int64_t>(state) << ':' << bool(i%2);
    reference << state << ':' << static_cast<int64_t>(state) << ':' << bool(i%2);
    assert(writer.bytes==reference.str());
  }
  writer.bytes.clear();
  writer << char(0) << static_cast<unsigned char>(255) << static_cast<signed char>(-1);
  assert(writer.bytes==std::string("\0\xff\xff",3));
}

static void frames() {
  for(auto size : {0u,1u,1023u,1024u,1025u,2048u,2049u,65537u}) {
    const auto plain=payload(size);
    for(bool paired : {false,true}) {
      HAPClient hap;
      if(paired)hap.cPair=&hap;
      for(unsigned i=0;i<sizeof hap.a2cKey;i++)hap.a2cKey[i]=uint8_t(i*7);
      HapOut out;
      // The counting pass must never emit data or consume a session nonce.
      out.write(plain.data(),plain.size());
      assert(out.getSize()==plain.size());out.flush();hashEquals(out,plain);
      assert(hap.client.records.empty() && out.getSize()==0);
      out.setHapClient(&hap);
      for(size_t offset=0;offset<plain.size();) {
        const auto n=std::min(size_t(137),plain.size()-offset);
        out.write(plain.data()+offset,n);offset+=n;
      }
      assert(out.getSize()==plain.size());out.flush();hashEquals(out,plain);
      const size_t expected=(size+1023)/1024;
      assert(hap.client.records.size()==expected && !hap.client.stopped);
      std::string received;uint8_t nonce[12]={};
      for(const auto &record : hap.client.records) {
        if(paired)received+=decrypt(hap,record,nonce);
        else {
          assert(!record.empty() && record.size()<=1024);
          received.append(reinterpret_cast<const char *>(record.data()),record.size());
        }
      }
      assert(received==plain);
      assert(!memcmp(hap.a2cNonce.get(),nonce,12));
      out.flush(); // exact-frame and repeated empty flush add no record/nonce
      assert(hap.client.records.size()==expected);
      assert(!memcmp(hap.a2cNonce.get(),nonce,12));
      hashEquals(out,"");
    }
  }
}

static void tlv() {
  for(auto size : {0u,1u,254u,255u,256u,510u,511u,1024u,2048u}) {
    const auto bytes=payload(size);
    TLV8 list;
    list.add(1,size/2,reinterpret_cast<const uint8_t *>(bytes.data()));
    list.add(1,size-size/2,reinterpret_cast<const uint8_t *>(bytes.data()+size/2));
    list.add(255);
    list.add(2,uint64_t(0xabcdef));
    std::string expected;
    size_t offset=0;
    do {
      const auto n=std::min(size_t(255),size-offset);
      expected.push_back(1);expected.push_back(char(n));
      expected.append(bytes,offset,n);offset+=n;
    } while(offset<size);
    expected.append("\xff\0\x02\x04\xef\xcd\xab\0",8);
    Collect collected;list.osprint(collected);
    assert(collected.bytes==expected && list.pack_size()==expected.size());
    HAPClient hap;hap.cPair=&hap;
    HapOut out;list.osprint(out);assert(out.getSize()==expected.size());out.flush();
    out.setHapClient(&hap);list.osprint(out);out.flush();hashEquals(out,expected);
    uint8_t nonce[12]={};std::string received;
    for(const auto &record:hap.client.records)received+=decrypt(hap,record,nonce);
    assert(received==expected);
  }
}

struct Callback {
  std::string bytes;
  unsigned completions=0;
  static void collect(const char *p,void *user) {
    auto &self=*static_cast<Callback *>(user);
    if(p)self.bytes+=p;
    else self.completions++;
  }
};
static void callback() {
  HapOut out;Callback called;
  const std::string json(2048,'x');
  out.setCallback(Callback::collect).setCallbackUserData(&called);
  out.write(json.data(),json.size());
  assert(out.getSize()==json.size());out.flush();
  assert(called.bytes==json && called.completions==1);hashEquals(out,json);
  out << "next";out.flush();hashEquals(out,"next");
  assert(called.bytes==json && called.completions==1);
  out.setCallback(Callback::collect).setCallbackUserData(&called);out.flush();
  assert(called.bytes==json && called.completions==2);hashEquals(out,"");
}
static void failures() {
  const auto plain=payload(3000);
  for(unsigned mode=0;mode<4;mode++) {
    HapOut out;HAPClient hap;
    if(mode!=3)hap.cPair=&hap;
    if(mode==0 || mode==3)hap.client.maxWrite=7;
    if(mode==1)encryptFailure=true;
    if(mode==2)memset(hap.a2cNonce.get()+4,255,8);
    out.setHapClient(&hap);out.write(plain.data(),plain.size());
    assert(out.getSize()==plain.size());out.flush();encryptFailure=false;
    assert(hap.client.stopped);
    assert(hap.client.records.size()==(mode==1?0:1));
    HAPClient fresh;fresh.cPair=&fresh;
    out.setHapClient(&fresh);out << "ok";out.flush();
    uint8_t nonce[12]={};
    assert(fresh.client.records.size()==1 && decrypt(fresh,fresh.client.records[0],nonce)=="ok");
  }
}
int main(int argc,char **argv) {
  assert(argc==2);
  const std::string scenario=argv[1];
  if(scenario=="numbers")numbers();
  else if(scenario=="frames")frames();
  else if(scenario=="tlv")tlv();
  else if(scenario=="callback")callback();
  else if(scenario=="failures")failures();
  else assert(false);
}
