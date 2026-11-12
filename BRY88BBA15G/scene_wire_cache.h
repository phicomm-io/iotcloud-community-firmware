#pragma once
#include <cstdlib>
#include <memory>
extern "C" {
#include "protocol/scene.h"
}
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif

struct SceneWordAllocation {
  static uint8_t *scratch(size_t bytes) {
    return static_cast<uint8_t *>(std::malloc(bytes));
  }
  static uint32_t *allocate(size_t bytes, bool &iram) {
#ifdef ARDUINO_ARCH_ESP32
    // Executable capability selects the unused instruction-memory heap first.
    // Its storage must never reach a byte-oriented codec, memcpy or String.
    auto *p = static_cast<uint32_t *>(heap_caps_malloc(
        bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC | MALLOC_CAP_32BIT));
    iram = p != nullptr;
    if (p) return p;
    return static_cast<uint32_t *>(heap_caps_malloc(
        bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
    iram = false;
    return static_cast<uint32_t *>(std::malloc(bytes));
#endif
  }
  static void release(void *p) { std::free(p); }
};

// One application owner. Cache the immutable SCN3 wire image in word-only RAM;
// byte-addressable scratch exists only while the unchanged codec encodes it.
// Volatile word accesses prevent the compiler from introducing byte loads or
// stores to ESP32 IRAM, including for an unaligned requested chunk boundary.
template<class Allocation = SceneWordAllocation> class SceneWireCache {
  struct ScratchDelete {
    void operator()(uint8_t *p) const { Allocation::release(p); }
  };
  uint32_t *words_ = nullptr;
  uint32_t revision_ = 0, crc_ = 0;
  bool iram_ = false;
public:
  SceneWireCache() = default;
  SceneWireCache(const SceneWireCache &) = delete;
  SceneWireCache &operator=(const SceneWireCache &) = delete;
  ~SceneWireCache() { Allocation::release(words_); }
  bool ready(const screen_scene &scene) const {
    return words_ && revision_ == scene.revision;
  }
  size_t size() const { return words_ ? SCENE_BYTES : 0; }
  uint32_t crc() const { return crc_; }
  const char *storage() const {
    return !words_ ? "unallocated" : iram_ ? "iram" : "dram";
  }
  bool prepare(const screen_scene &scene) {
    if (ready(scene)) return true;
    std::unique_ptr<uint8_t[], ScratchDelete> bytes(Allocation::scratch(SCENE_BYTES));
    if (!bytes || !scene_encode(&scene, bytes.get())) return false;
    const uint32_t crc = scene_crc32(bytes.get(), SCENE_BYTES);
    if (!words_) {
      words_ = Allocation::allocate((SCENE_BYTES + 3u) & ~size_t(3u), iram_);
      if (!words_) return false;
    }
    volatile uint32_t *out = words_;
    for (size_t at = 0; at < SCENE_BYTES; at += 4) {
      uint32_t word = 0;
      for (unsigned k = 0; k < 4 && at + k < SCENE_BYTES; k++)
        word |= uint32_t(bytes[at + k]) << (8 * k);
      out[at / 4] = word;
    }
    revision_ = scene.revision;
    crc_ = crc;
    return true;
  }
  bool read(size_t offset, uint8_t *out, size_t length) const {
    if (!words_ || (length && !out) || offset > SCENE_BYTES ||
        length > SCENE_BYTES - offset) return false;
    const volatile uint32_t *in = words_;
    while (length) {
      const uint32_t word = in[offset / 4];
      unsigned k = offset % 4;
      do {
        *out++ = uint8_t(word >> (8 * k++));
        ++offset;
        --length;
      } while (length && k < 4);
    }
    return true;
  }
};
