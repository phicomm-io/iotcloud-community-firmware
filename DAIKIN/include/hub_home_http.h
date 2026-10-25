/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace iotcloud {
// HAP uses a 64-bit little-endian counter after four zero padding bytes.
// Exhaustion requires a new session; wrapping would reuse an AEAD nonce.
inline bool homeNonceIncrement(uint8_t (&nonce)[12]) {
  for(unsigned i=4;i<12;++i)
    if(++nonce[i])return true;
  return false;
}

// One bounded request at a time; HTTP pipelining is deliberately rejected.
// Body bytes are opaque: pairing TLV8 can contain every byte, including NUL.
class HomeRequest {
  static bool token(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c && std::strchr("!#$%&'*+-.^_`|~", c));
  }
  static bool matches(const std::string &s, size_t first, size_t last,
                      const char *lower) {
    if (last - first != std::strlen(lower)) return false;
    for (; first < last; ++first, ++lower) {
      auto c = static_cast<unsigned char>(s[first]);
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if (c != static_cast<unsigned char>(*lower)) return false;
    }
    return true;
  }
  bool headers(size_t end) {
    const auto line = bytes.find("\r\n");
    if (line == std::string::npos || line > end) return false;
    const auto method = bytes.find(' ');
    const auto target = bytes.find(' ', method + 1);
    if (method == std::string::npos || target == std::string::npos ||
        !method || target <= method + 1 || bytes[method + 1] != '/' ||
        bytes.compare(target, line - target, " HTTP/1.1") != 0) return false;
    for (size_t i = 0; i < method; ++i)
      if (bytes[i] < 'A' || bytes[i] > 'Z') return false;
    for (size_t i = method + 1; i < target; ++i)
      if (static_cast<unsigned char>(bytes[i]) <= 0x20 ||
          static_cast<unsigned char>(bytes[i]) >= 0x7f) return false;
    size_t length = 0;
    bool foundLength = false, foundType = false;
    for (auto pos = line + 2; pos < end;) {
      const auto next = bytes.find("\r\n", pos);
      const auto colon = bytes.find(':', pos);
      if (next == std::string::npos || next > end || colon == pos ||
          colon == std::string::npos || colon >= next) return false;
      for (auto i = pos; i < colon; ++i)
        if (!token(bytes[i])) return false;
      for (auto i = colon + 1; i < next; ++i) {
        auto c = static_cast<unsigned char>(bytes[i]);
        if ((c < 0x20 && c != '\t') || c >= 0x7f) return false;
      }
      auto first = bytes.find_first_not_of(" \t", colon + 1);
      if (first == std::string::npos || first > next) first = next;
      auto last = next;
      while (last > first && (bytes[last - 1] == ' ' || bytes[last - 1] == '\t')) --last;
      if (matches(bytes, pos, colon, "transfer-encoding")) return false;
      if (matches(bytes, pos, colon, "content-length")) {
        if (foundLength || first == last) return false;
        foundLength = true;
        for (auto i = first; i < last; ++i) {
          if (bytes[i] < '0' || bytes[i] > '9') return false;
          const auto digit = unsigned(bytes[i] - '0');
          if (length > (Limit - digit) / 10) return false;
          length = length * 10 + digit;
        }
      } else if (matches(bytes, pos, colon, "content-type")) {
        if (foundType) return false;
        foundType = true;
        type = matches(bytes, first, last, "application/pairing+tlv8") ? Pairing :
               matches(bytes, first, last, "application/hap+json") ? Json : Other;
      }
      pos = next + 2;
    }
    expected = end + 4 + length;
    return expected <= Limit && bytes.size() <= expected;
  }
public:
  static constexpr size_t Limit = 8096, HeaderLimit = 2048;
  enum Type { Other, Pairing, Json };
  std::string bytes;
  size_t expected = 0;
  Type type = Other;
  bool (*allocate)(size_t) = nullptr;
  bool append(const uint8_t *data, size_t n) {
    if (n > Limit - bytes.size() ||
        (expected && n > expected - bytes.size())) return false;
    if (bytes.size() + n > bytes.capacity()) {
      const auto capacity = std::min(size_t(Limit), std::max(bytes.size() + n,
          std::max(size_t(32), bytes.capacity() * 2)));
      // Check the full replacement, including its NUL, while the old buffer
      // is still live. reserve() on the old string may silently double past
      // Limit when only the last byte of an otherwise valid request arrives.
      if (allocate && !allocate(capacity + 1)) return false;
      std::string replacement;
      replacement.reserve(capacity);
      replacement.assign(bytes);
      bytes.swap(replacement);
    }
    if (n) bytes.append(reinterpret_cast<const char *>(data), n);
    if (!expected) {
      const auto end = bytes.find("\r\n\r\n");
      if (end == std::string::npos) return bytes.size() <= HeaderLimit;
      if (end + 4 > HeaderLimit || !headers(end)) return false;
    }
    return true;
  }
  bool complete() const { return expected && bytes.size() == expected; }
  void clear() { bytes.clear(); expected = 0; type = Other; }
};

// Pairing messages need at most a 384-byte SRP key and a few small fields.
// Preflight before TLV8 allocates one list node per field. Fragmented fields
// may repeat only directly after a full 255-byte fragment of the same tag.
inline bool homeTlvValid(const uint8_t *data, size_t size) {
  if (!size || size > 1024) return false;
  uint8_t seen[32] = {};
  unsigned previous = 256, previousLength = 0, fields = 0;
  while (size) {
    if (size < 2 || ++fields > 16) return false;
    const auto tag = data[0], length = data[1];
    if (size_t(length) > size - 2 ||
        ((seen[tag / 8] & (1u << (tag % 8))) &&
         (previous != tag || previousLength != 255))) return false;
    seen[tag / 8] |= 1u << (tag % 8);
    previous = tag;
    previousLength = length;
    data += length + 2;
    size -= length + 2;
  }
  return true;
}

// Buffer exactly one HAP encrypted record. Reading only remaining() bytes
// keeps coalesced records in TCP until the previous request is dispatched.
class HomeRecord {
public:
  static constexpr size_t Limit = 1024, Overhead = 18;
  std::vector<uint8_t> bytes;
  bool (*allocate)(size_t) = nullptr;
  size_t length() const { return bytes.size() < 2 ? 0 : bytes[0] + 256u * bytes[1]; }
  size_t remaining() const { return (bytes.size() < 2 ? 2 : length() + Overhead) - bytes.size(); }
  bool append(const uint8_t *data, size_t n) {
    if (n > remaining() || n > Limit + Overhead - bytes.size()) return false;
    if (bytes.size() + n > bytes.capacity()) {
      const auto capacity = std::min(Limit + Overhead,
          std::max(bytes.size() + n, std::max(size_t(32), bytes.capacity() * 2)));
      if (allocate && !allocate(capacity)) return false;
      std::vector<uint8_t> replacement;
      replacement.reserve(capacity);
      replacement.assign(bytes.begin(), bytes.end());
      bytes.swap(replacement);
    }
    if (n) bytes.insert(bytes.end(), data, data + n);
    return bytes.size() < 2 || (length() && length() <= Limit);
  }
  bool complete() const { return bytes.size() >= 2 && !remaining(); }
  void clear() { bytes.clear(); }
};

// Bound both idle gaps and the lifetime of a partially received request.
class HomeInputWindow {
  uint32_t began = 0, last = 0;
  bool partial = false;
public:
  void opened(uint32_t now) { last = now; partial = false; }
  void received(uint32_t now) {
    if (!partial) { began = now; partial = true; }
    last = now;
  }
  void completed() { partial = false; }
  bool expired(uint32_t now, bool unverified) const {
    return ((partial || unverified) && uint32_t(now - last) > 3000) ||
           (partial && uint32_t(now - began) > 10000);
  }
};
} // namespace iotcloud
