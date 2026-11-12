#pragma once
#include <ArduinoJson.h>
extern "C" {
#include "protocol/services.h"
}
namespace bry {
bool weatherDecode(const uint8_t *, size_t, DynamicJsonDocument &);
bool weatherCurrent(const char *provider, JsonVariantConst, screen_weather &);
uint8_t weatherStockCode(const char *provider, JsonVariantConst);
bool weatherStockFields(const screen_weather &, uint8_t code, uint32_t now,
                        JsonObject);
bool weatherForecast(const char *provider, JsonVariantConst, screen_weather &,
                     int timezone);
void weatherWire(const screen_weather &, uint8_t out[353]);
void textCopy(char *, size_t, const char *);
} // namespace bry
