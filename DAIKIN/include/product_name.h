/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstring>
namespace daikin {
// Keep user names; specialize only the historical generic default.
inline const char *productName(const char *saved = nullptr) {
  if (saved && *saved && std::strcmp(saved, "DAIKIN")) return saved;
#ifdef IOTCLOUD_BRY88_SCREEN
  return "DAIKIN G3";
#else
  return "DAIKIN G1/G2";
#endif
}
}
