#include "scene_wire_cache.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <vector>

struct TestAllocation {
  static bool failScratch, failWords, iram;
  static unsigned scratchCalls, wordCalls, liveScratch, liveWords;
  static void *wordAddress;
  static uint8_t *scratch(size_t bytes) {
    ++scratchCalls;
    if (failScratch) return nullptr;
    auto *p = static_cast<uint8_t *>(std::malloc(bytes));
    assert(p); ++liveScratch; return p;
  }
  static uint32_t *allocate(size_t bytes, bool &instruction) {
    ++wordCalls;
    if (failWords) return nullptr;
    instruction = iram;
    wordAddress = std::malloc(bytes);
    assert(wordAddress && !(reinterpret_cast<uintptr_t>(wordAddress) & 3));
    ++liveWords; return static_cast<uint32_t *>(wordAddress);
  }
  static void release(void *p) {
    if (!p) return;
    if (p == wordAddress) { --liveWords; wordAddress = nullptr; }
    else --liveScratch;
    std::free(p);
  }
};
bool TestAllocation::failScratch=false, TestAllocation::failWords=false,
     TestAllocation::iram=true;
unsigned TestAllocation::scratchCalls=0, TestAllocation::wordCalls=0,
         TestAllocation::liveScratch=0, TestAllocation::liveWords=0;
void *TestAllocation::wordAddress=nullptr;

static void checkWire(SceneWireCache<TestAllocation> &cache,
                      const screen_scene &scene) {
  std::vector<uint8_t> expected(SCENE_BYTES), actual(SCENE_BYTES);
  assert(scene_encode(&scene,expected.data()));
  assert(cache.crc()==scene_crc32(expected.data(),expected.size()));
  assert(cache.read(0,actual.data(),actual.size()) && actual==expected);
  // Same 512-byte windows as UART sync, including its short final block.
  for (size_t at=0;at<SCENE_BYTES;at+=512) {
    const size_t n=std::min(size_t(512),size_t(SCENE_BYTES-at));
    assert(cache.read(at,actual.data()+at,n));
  }
  assert(actual==expected);
  // HTTP accepts arbitrary offsets and lengths, not just aligned windows.
  for (size_t at : {0u,1u,2u,3u,4u,127u,511u,512u,SCENE_BYTES-1u})
    for (size_t n : {0u,1u,2u,3u,4u,255u,511u,512u}) {
      if (n>SCENE_BYTES-at) continue;
      uint8_t out[514]; std::memset(out,0xA5,sizeof(out));
      assert(cache.read(at,out+1,n));
      assert(out[0]==0xA5 && out[n+1]==0xA5);
      assert(!std::memcmp(out+1,expected.data()+at,n));
    }
  assert(cache.read(SCENE_BYTES,nullptr,0));
  assert(!cache.read(SCENE_BYTES,nullptr,1));
  assert(!cache.read(SCENE_BYTES+1,nullptr,0));
  assert(!cache.read(0,nullptr,1));
  assert(!cache.read(1,actual.data(),std::numeric_limits<size_t>::max()));
}

int main() {
  screen_scene scene; scene_defaults(&scene);
  std::strcpy(scene.page[0].title,"广州天气");
  std::strcpy(scene.item[0].text,"A\n广州 ");
  {
    SceneWireCache<TestAllocation> cache;
    assert(!cache.size() && !cache.ready(scene));
    assert(!cache.read(0,nullptr,0));
    TestAllocation::failWords=true;
    assert(!cache.prepare(scene));
    assert(!cache.size() && !TestAllocation::liveScratch && !TestAllocation::liveWords);
    TestAllocation::failWords=false;
    assert(cache.prepare(scene));
    assert(cache.size()==SCENE_BYTES && !std::strcmp(cache.storage(),"iram"));
    assert(!TestAllocation::liveScratch && TestAllocation::liveWords==1);
    checkWire(cache,scene);

    const auto calls=TestAllocation::scratchCalls;
    TestAllocation::failScratch=TestAllocation::failWords=true;
    assert(cache.prepare(scene)); // No transient allocation for unchanged data.
    assert(TestAllocation::scratchCalls==calls);
    checkWire(cache,scene);
    screen_scene next=scene; ++next.revision;
    assert(!cache.prepare(next)); // Failed refresh preserves the old snapshot.
    assert(cache.ready(scene) && !cache.ready(next));
    checkWire(cache,scene);
    TestAllocation::failScratch=false;
    next.pages=0;
    assert(!cache.prepare(next) && cache.ready(scene));
    assert(!TestAllocation::liveScratch);
    next=scene; ++next.revision;
    std::strcpy(next.page[0].title,"Changed");
    assert(cache.prepare(next)); // Existing word storage can be reused under OOM.
    assert(cache.ready(next) && !cache.ready(scene));
    checkWire(cache,next);
    assert(TestAllocation::liveWords==1 && !TestAllocation::liveScratch);
  }
  assert(!TestAllocation::liveWords && !TestAllocation::liveScratch);
  TestAllocation::failWords=false; TestAllocation::iram=false;
  {
    SceneWireCache<TestAllocation> fallback;
    assert(fallback.prepare(scene) && !std::strcmp(fallback.storage(),"dram"));
    checkWire(fallback,scene);
  }
  assert(!TestAllocation::liveWords && !TestAllocation::liveScratch);
}
