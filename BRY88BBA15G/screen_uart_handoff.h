/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
namespace bry {
// Called after ordinary writers stop, before the first raw OTA request. Wait
// for complete in-flight frames rather than starting halfway through one.
template<class Port, class Clock, class Pause>
bool uartDiscardUntilQuiet(Port &port, Clock clock, Pause pause) {
  const uint32_t start = clock();
  uint32_t last = start;
  do {
    unsigned budget = 8192;
    while (budget-- && port.available()) {
      port.read();
      last = clock();
    }
    if (uint32_t(clock() - last) >= 30) return true;
    pause(1);
  } while (uint32_t(clock() - start) < 500);
  return false;
}
}
