/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <ArduinoJson.h>
#include "hub_device_bus.h"
namespace iotcloud {
// JsonString::size includes embedded NUL bytes; strlen alone would accept a
// truncated name. Match the bus's UTF-8 policy before saving or forwarding.
inline bool deviceName(JsonVariantConst value, size_t maximum = 63) {
  if (!value.is<const char *>()) return false;
  auto text = value.as<JsonString>();
  return text.size() && text.size() <= maximum &&
         HubDeviceBus::utf8(std::string(text.c_str(), text.size()));
}
}
