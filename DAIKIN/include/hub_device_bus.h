/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace iotcloud {
// Synchronous application routing, not an MQTT broker. Route keys are retained
// solely for external cloud mappings. No clients, sockets, subscriptions,
// retained storage or replay exist here. Handlers borrowing body/route must copy
// them before queuing asynchronous UDP work. retain is an upstream intent only.
class HubDeviceBus {
public:
  static constexpr size_t Packet=8192, Topic=512;
  enum class Origin {Local, Upstream, Bemfa, Internal};
  using Dispatch=std::function<bool(const std::string &,const std::string &,bool,Origin)>;
  Dispatch dispatch;
  bool publish(const std::string &route,const std::string &body,bool retain,Origin origin){
    return body.size()<=Packet && topicValid(route) && dispatch && dispatch(route,body,retain,origin);
  }
  static bool utf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
      uint32_t c = uint8_t(s[i++]);
      unsigned n = 0;
      uint32_t minimum = 0;
      if (c >= 0xc2 && c <= 0xdf) {
        n = 1;
        c &= 31;
        minimum = 0x80;
      } else if (c >= 0xe0 && c <= 0xef) {
        n = 2;
        c &= 15;
        minimum = 0x800;
      } else if (c >= 0xf0 && c <= 0xf4) {
        n = 3;
        c &= 7;
        minimum = 0x10000;
      } else if (c >= 0x80)
        return false;
      if (i + n > s.size())
        return false;
      while (n--) {
        uint8_t x = s[i++];
        if ((x & 0xc0) != 0x80)
          return false;
        c = c * 64 + (x & 63);
      }
      if (c < minimum || !c || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
          (c >= 0xfdd0 && c <= 0xfdef) || (c & 0xffff) == 0xfffe ||
          (c & 0xffff) == 0xffff)
        return false;
    }
    return true;
  }
  static bool topicValid(const std::string &s, bool filter = false) {
    if (s.empty() || s.size() > Topic || !utf8(s))
      return false;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '#' &&
          (!filter || i + 1 != s.size() || (i && s[i - 1] != '/')))
        return false;
      if (s[i] == '+' && (!filter || (i && s[i - 1] != '/') ||
                          (i + 1 < s.size() && s[i + 1] != '/')))
        return false;
    }
    return true;
  }
};
} // namespace iotcloud
