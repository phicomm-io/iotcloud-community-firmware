#pragma once
#include <ArduinoJson.h>
#include <cstring>
extern "C" {
#include "store.h"
}

// ArduinoJson emits binary bytes, including NULs. Buffer small writes without
// holding a second complete MessagePack record beside the SCN3 layout text.
class ScreenConfigWriter {
  bry_store_sink sink;
  void *target;
  uint8_t bytes[256];
  size_t used = 0;
  bool failed = false;
public:
  ScreenConfigWriter(bry_store_sink sink, void *target): sink(sink), target(target) {}
  bool flush() {
    if (failed) return false;
    if (used && !sink(target, bytes, used)) {
      failed = true;
      return false;
    }
    used = 0;
    return true;
  }
  size_t write(uint8_t value) { return write(&value, 1); }
  size_t write(const uint8_t *data, size_t size) {
    if (failed) return 0;
    size_t written = 0;
    while (written < size) {
      size_t count = sizeof(bytes) - used;
      if (count > size - written) count = size - written;
      std::memcpy(bytes + used, data + written, count);
      used += count;
      written += count;
      if (used == sizeof(bytes) && !flush()) return 0;
    }
    return written;
  }
};
static bool screenProduceConfig(void *context, bry_store_sink sink, void *target) {
  const JsonDocument &doc = *static_cast<const JsonDocument *>(context);
  ScreenConfigWriter writer(sink, target);
  return serializeMsgPack(doc, writer) == measureMsgPack(doc) && writer.flush();
}
static bool screenPersistConfig(bry_store *store, const JsonDocument &doc) {
  return bry_store_save_stream(store, measureMsgPack(doc), screenProduceConfig,
                               const_cast<JsonDocument *>(&doc));
}
