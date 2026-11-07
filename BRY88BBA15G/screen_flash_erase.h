#pragma once
#include <cstddef>
#include <cstdint>

// Never hold the flash in a multi-sector erase for a whole staged image.
// Each completed sector yields before the next; an interrupted upload keeps
// its header invalid, while the separate durable configuration stays intact.
template<class Erase, class Yield>
static bool screenEraseSectors(uint32_t address, size_t size, size_t capacity,
                               Erase erase, Yield yield) {
  constexpr size_t sector = 4096;
  if (!size || address % sector || size % sector || address > capacity ||
      size > capacity - address)
    return false;
  while (size) {
    if (!erase(address, sector)) return false;
    address += sector;
    size -= sector;
    yield();
  }
  return true;
}

// Erase payload sectors only as contiguous HTTP bytes arrive. This avoids the
// erase burst before receiving a complete screen image. The separate image
// header must be invalidated before starting and committed only after hashing.
class ScreenUploadErase {
  uint32_t base_, size_, erased_ = 0, received_ = 0;
  bool valid_;
public:
  ScreenUploadErase(uint32_t base, uint32_t size, size_t capacity)
      : base_(base), size_(size),
        valid_(size && !(base % 4096) && !(size % 4096) &&
               base <= capacity && size <= capacity - base) {}
  template<class Erase>
  bool prepare(uint32_t offset, size_t bytes, Erase erase) {
    if (!valid_ || offset != received_ || !bytes || bytes > size_ - received_)
      return false;
    uint32_t end = received_ + bytes;
    while (erased_ < end) {
      if (!erase(base_ + erased_, 4096)) return false;
      erased_ += 4096;
    }
    received_ = end;
    return true;
  }
};
