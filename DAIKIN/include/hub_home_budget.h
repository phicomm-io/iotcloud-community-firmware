/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstddef>
#include <cstring>

namespace iotcloud {
// Conservative startup admission, not a replacement for fallible allocation.
// Sizes come from the firmware ABI. Every block includes alignment and 16
// bytes of allocator allowance; strings include both value and newValue.
class HomeGraphBudget {
public:
  enum Kind { Invalid, M1, TC1, A1, Daikin };
  struct Sizes {
    size_t accessory, characteristic, information, sensor, outlet, fan, light, pointer;
  };
  static constexpr size_t BusinessReserve = 32768, Bootstrap = 8192,
                          GrowthReserve = 1024, MinimumBlock = 20480;
  size_t bytes = Bootstrap + GrowthReserve;
  size_t largest = MinimumBlock;
  size_t accessories = 0, services = 0, characteristics = 0, devices = 0, loops = 0;
  bool valid = true;

  explicit HomeGraphBudget(Sizes sizes) : sizes(sizes) {
    if (!sizes.accessory || !sizes.characteristic || !sizes.information ||
        !sizes.sensor || !sizes.outlet || !sizes.fan || !sizes.light || !sizes.pointer)
      valid = false;
    information(1, 12, 12); // Bridge: IoTCloud Hub.
  }
  static Kind kind(const char *model) {
    if (!model) return Invalid;
    return !std::strcmp(model, "m1") ? M1 : !std::strcmp(model, "tc1") ? TC1 :
           !std::strcmp(model, "a1") ? A1 : !std::strcmp(model, "daikin") ? Daikin : Invalid;
  }
  bool device(Kind model) {
    if (finished || model < M1 || model > Daikin || devices >= 13) return valid = false;
    ++devices;
    const size_t length = model == Daikin ? 6 : model == TC1 ? 3 : 2;
    information(model == TC1 ? 7 : model == A1 ? 3 : model == Daikin ? 6 : 5,
                length + 7, length);
    // The owner name is at most 32 bytes. Its hexadecimal MQTT user key plus
    // topic/model/MAC fits 97 characters; libstdc++ growth fits this allowance.
    block(256);
    if (model == TC1) {
      for (unsigned i = 0; i < 6; ++i) {
        service(sizes.outlet, 2, 2);
        text(8); // "Outlet 1" through "Outlet 6".
      }
    } else if (model == A1) {
      service(sizes.fan, 2, 8);
      service(sizes.light, 2, 8);
    } else {
      service(sizes.sensor, 3, 2); // Temperature.
      service(sizes.sensor, 3, 2); // Humidity.
      service(sizes.sensor, 4, 4); // CO2 (optional vector rounds 3 to 4).
      service(sizes.sensor, 4, 4); // PM2.5.
      if (model == Daikin) service(sizes.sensor, 4, 4); // PM10.
    }
    return valid;
  }
  bool fits(size_t available, size_t blockSize) {
    if (!finished) {
      vector(accessories);
      vector(loops);
      reserve(BusinessReserve);
      finished = true;
    }
    return valid && available >= bytes && blockSize >= largest;
  }

private:
  Sizes sizes;
  bool finished = false;
  void reserve(size_t n) {
    if (n > size_t(-1) - bytes) valid = false;
    else bytes += n;
  }
  void block(size_t n, size_t count = 1) {
    if (!valid || !count || !n) return;
    if (n > size_t(-1) - 19) { valid = false; return; }
    const size_t cost = ((n + 3) & ~size_t(3)) + 16;
    if (count > (size_t(-1) - bytes) / cost) { valid = false; return; }
    bytes += cost * count;
    if (n > largest) largest = n;
  }
  void vector(size_t count) {
    if (!valid || !count) return;
    size_t capacity = 1;
    while (capacity < count) {
      if (capacity > size_t(-1) / 2) { valid = false; return; }
      capacity *= 2;
    }
    if (capacity > size_t(-1) / sizes.pointer) { valid = false; return; }
    block(capacity * sizes.pointer);
  }
  void text(size_t length) { block(length + 1, 2); }
  void information(size_t count, size_t nameLength, size_t modelLength) {
    ++accessories; ++services; characteristics += 6;
    block(sizes.accessory);
    block(sizes.information);
    block(sizes.characteristic, 6);
    vector(count); // Accessory Services.
    vector(6); vector(1); vector(7); // Information characteristics/req/opt.
    text(nameLength); text(8); text(modelLength); text(12); text(5);
  }
  void service(size_t object, size_t chars, size_t optional) {
    ++services; ++loops; characteristics += chars;
    block(object);
    block(sizes.characteristic, chars);
    vector(chars); vector(1); vector(optional);
  }
};
} // namespace iotcloud
