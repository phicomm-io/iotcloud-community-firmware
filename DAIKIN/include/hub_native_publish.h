/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstring>
#include <string>

namespace iotcloud {
// Borrow the namespace patterns while measuring and rendering a native message.
// The caller admits and reserves the entire result before appending any bytes.
struct NativeRule {
  const std::string &from, &to;
};
inline bool nativeMessageSize(const char *input, size_t length,
                              const NativeRule *rules, size_t count,
                              size_t limit, size_t &result) {
  if (!input || length > limit) return false;
  result = 0;
  for (size_t at = 0; at < length;) {
    size_t take = 1, put = 1;
    for (size_t i = 0; i < count; ++i) {
      const auto &rule = rules[i];
      if (!rule.from.empty() && rule.from.size() <= length - at &&
          !std::memcmp(input + at, rule.from.data(), rule.from.size())) {
        take = rule.from.size(); put = rule.to.size(); break;
      }
    }
    if (put > limit - result) return false;
    result += put; at += take;
  }
  return true;
}
inline void nativeMessageAppend(std::string &out, const char *input,
                                size_t length, const NativeRule *rules,
                                size_t count) {
  size_t literal = 0;
  for (size_t at = 0; at < length;) {
    size_t matched = count;
    for (size_t i = 0; i < count; ++i) {
      const auto &rule = rules[i];
      if (!rule.from.empty() && rule.from.size() <= length - at &&
          !std::memcmp(input + at, rule.from.data(), rule.from.size())) {
        matched = i; break;
      }
    }
    if (matched == count) { ++at; continue; }
    out.append(input + literal, at - literal);
    out.append(rules[matched].to);
    at += rules[matched].from.size(); literal = at;
  }
  out.append(input + literal, length - literal);
}
} // namespace iotcloud
