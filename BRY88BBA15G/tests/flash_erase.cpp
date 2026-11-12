#include "screen_flash_erase.h"
#include "store.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static unsigned char flash[BRY_STORE_SIZE];
int main() {
  unsigned erased=0, yielded=0;
  auto erase=[&](uint32_t address,size_t size) {
    assert(size==4096 && address==BRY_IMAGE_HEADER+erased*4096);
    ++erased;std::memset(flash+address,255,size);return true;
  };
  auto yield=[&] { assert(erased==yielded+1);++yielded; };
  assert(screenEraseSectors(BRY_IMAGE_HEADER,0x68000,sizeof(flash),erase,yield));
  assert(erased==104 && yielded==104);
  for(unsigned i=0;i<sizeof(flash);i++)
    assert(flash[i]==(i>=BRY_IMAGE_HEADER && i<BRY_IMAGE_HEADER+0x68000?255:0));
  auto unchanged=[&](uint32_t address,size_t size) { (void)address;(void)size;assert(false);return false; };
  assert(!screenEraseSectors(1,4096,sizeof(flash),unchanged,yield));
  assert(!screenEraseSectors(4096,4095,sizeof(flash),unchanged,yield));
  assert(!screenEraseSectors(0,0,sizeof(flash),unchanged,yield));
  assert(!screenEraseSectors(sizeof(flash),4096,sizeof(flash),unchanged,yield));
  assert(!screenEraseSectors(0xfffff000u,8192,sizeof(flash),unchanged,yield));
  // A failed sector aborts immediately without erasing later/config sectors.
  std::memset(flash,0,sizeof(flash));erased=yielded=0;
  assert(!screenEraseSectors(BRY_IMAGE_HEADER,0x68000,sizeof(flash),
      [&](uint32_t address,size_t size) { return erased!=3 && erase(address,size); },yield));
  assert(erased==3 && yielded==3 && flash[0x4000]==0 && flash[0x80000]==0);
  // Receive arbitrary HTTP fragment sizes across every sector boundary.
  // Existing payload bytes must survive later prepare calls; untouched sectors
  // and durable configuration must not be erased ahead of arriving data.
  std::memset(flash,0,sizeof(flash));erased=0;
  ScreenUploadErase stream(BRY_IMAGE_START,BRY_IMAGE_LIMIT,sizeof(flash));
  auto payloadErase=[&](uint32_t address,size_t bytes) {
    assert(address==BRY_IMAGE_START+erased*4096 && bytes==4096);
    ++erased;std::memset(flash+address,255,bytes);return true;
  };
  const unsigned chunks[]={1,777,1024,513,991,93};
  for(unsigned at=0,i=0;at<BRY_IMAGE_LIMIT;++i) {
    unsigned n=chunks[i%6];if(n>BRY_IMAGE_LIMIT-at)n=BRY_IMAGE_LIMIT-at;
    assert(stream.prepare(at,n,payloadErase));
    for(unsigned j=0;j<at;j++)assert(flash[BRY_IMAGE_START+j]==0x5a);
    std::memset(flash+BRY_IMAGE_START+at,0x5a,n);
    assert(erased==(at+n+4095)/4096);
    assert(flash[BRY_IMAGE_HEADER]==0 && flash[0x80000]==0);
    at+=n;
  }
  assert(erased==103);
  assert(!stream.prepare(BRY_IMAGE_LIMIT,1,unchanged));
  ScreenUploadErase invalid(1,4096,sizeof(flash));
  assert(!invalid.prepare(0,1,unchanged));
  ScreenUploadErase interrupted(BRY_IMAGE_START,BRY_IMAGE_LIMIT,sizeof(flash));
  assert(!interrupted.prepare(1,1,unchanged));
  assert(!interrupted.prepare(0,0,unchanged));
  assert(!interrupted.prepare(0,BRY_IMAGE_LIMIT+1,unchanged));
  unsigned calls=0;
  assert(!interrupted.prepare(0,4097,[&](uint32_t address,size_t) {
    assert(address==BRY_IMAGE_START+calls*4096);return ++calls!=2;
  }));
  assert(calls==2);
  puts("PASS paced/sequential erase, fragmented input, geometry/interruption guards, config isolation");
}
