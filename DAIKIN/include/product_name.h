/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstring>
namespace daikin {
// Keep user names; specialize only the historical generic default.
inline const char *productName(const char *saved = nullptr) {
  if (saved && *saved && std::strcmp(saved, "DAIKIN") && std::strcmp(saved, "DAIKIN G1/G2") && std::strcmp(saved, "DAIKIN G3")) return saved;
#ifdef IOTCLOUD_BRY88_SCREEN
  return "BRY88BBA15G";
#else
  return "BRY88AX151K";
#endif
}
}
