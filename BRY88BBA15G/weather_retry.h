#pragma once
#include <stdint.h>
#include <stddef.h>
/* TLS malloc needs byte-addressable DRAM. ESP.getFreeHeap also counts IRAM
 * which is only usable by aligned 32-bit loads. Never use it as a TLS budget. */
struct WeatherRetry {
  uint32_t next = 0, lastStart = 0;
  unsigned failures = 0;
  bool started = false;
  static bool memoryReady(size_t bytes, size_t largest, bool smallResponse) {
    return bytes >= (smallResponse ? 73728u : 86016u) && largest >= 24576u;
  }
  bool due(uint32_t now, bool manual) const {
    return (!started || uint32_t(now-lastStart)>=15000u) &&
           (manual || int32_t(now-next)>=0);
  }
  void begin(uint32_t now) { lastStart=now; started=true; }
  void failed(uint32_t now) {
    lastStart=now;started=true;
    if(failures<5) ++failures;
    const unsigned delay = failures>=5 ? 300u : (15u<<failures);
    next=now+delay*1000u;
  }
  void success(uint32_t now, uint32_t interval) { failures=0;next=now+interval*1000u; }
  uint32_t remaining(uint32_t now) const {return int32_t(next-now)>0?next-now:0;}
};
