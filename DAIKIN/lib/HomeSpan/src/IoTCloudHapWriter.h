// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace iotcloud {

// HAP needs byte writes and locale-independent decimal integers. Keeping this
// interface narrow avoids pulling the C++ stream/locale machinery into Flash.
class HapByteWriter {
 protected:
  ~HapByteWriter() = default;

 private:
  HapByteWriter &number(uint64_t magnitude, bool negative) {
    char digits[21];
    size_t first = sizeof(digits);
    do {
      digits[--first] = char('0' + magnitude % 10);
      magnitude /= 10;
    } while (magnitude);
    if (negative) digits[--first] = '-';
    write(digits + first, sizeof(digits) - first);
    return *this;
  }

 public:
  virtual void write(const char *data, size_t size) = 0;

  HapByteWriter &operator<<(const char *text) {
    if (text) write(text, std::strlen(text));
    return *this;
  }
  HapByteWriter &operator<<(char byte) {
    write(&byte, 1);
    return *this;
  }
  HapByteWriter &operator<<(signed char byte) { return *this << char(byte); }
  HapByteWriter &operator<<(unsigned char byte) { return *this << char(byte); }

  template <class T>
  typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value,
                          HapByteWriter &>::type
  operator<<(T value) {
    return number(uint64_t(value), false);
  }
  template <class T>
  typename std::enable_if<std::is_integral<T>::value && std::is_signed<T>::value,
                          HapByteWriter &>::type
  operator<<(T value) {
    const uint64_t bits = uint64_t(value);
    return number(value < 0 ? uint64_t(0) - bits : bits, value < 0);
  }

  // JSON characteristic floats already pass through uvPrint. Requiring that
  // path also prevents a future float from silently becoming one character.
  HapByteWriter &operator<<(float) = delete;
  HapByteWriter &operator<<(double) = delete;
  HapByteWriter &operator<<(long double) = delete;
};

}  // namespace iotcloud
