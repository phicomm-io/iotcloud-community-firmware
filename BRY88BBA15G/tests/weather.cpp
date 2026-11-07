#include "weather.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
static DynamicJsonDocument doc(12288);
static JsonVariantConst json(const char *s) {
  assert(bry::weatherDecode((const uint8_t *)s, strlen(s), doc));
  return doc.as<JsonVariantConst>();
}
int main() {
  screen_weather w = {};
  assert(bry::weatherCurrent(
      "qweather",
      json(
          R"({"condition":{"text":"晴"},"temperature":{"value":23,"unit":"°C"},"feelsLike":{"value":22,"unit":"°C"},"humidity":0.42,"wind":{"speed":{"value":2,"unit":"m/s"}}})"),
      w));
  assert(w.fields == 15 && fabs(w.humidity - 42) < .001);
  assert(bry::weatherForecast(
      "qweather",
      json(
          R"({"days":[{"forecastStartTime":"2026-11-05T00:00+08:00","astro":{"solarNoon":"2026-11-05T12:00+08:00"},"temperatureMin":{"value":10,"unit":"°C"},"temperatureMax":{"value":24,"unit":"°C"},"daytime":{"condition":{"text":"晴"}}}]})"),
      w, 480));
  w.observed = 1793880000;
  w.ttl = 1800000;
  std::strcpy(w.location, "杭州");
  uint8_t p[353];
  bry::weatherWire(w, p);
  screen_weather received = {};
  assert(weather_receive(&received, p, sizeof(p), 123));
  assert(received.ttl == 1800000 &&
         !strcmp(received.days[0].date, "2026-11-05") &&
         received.days[0].low == 10 && received.days[0].high == 24 &&
         !strcmp(received.location, "杭州"));
  assert(bry::weatherCurrent(
      "seniverse",
      json(
          R"({"results":[{"location":{"name":"Hangzhou"},"now":{"temperature":"18","text":"Cloudy"}}]})"),
      w));
  assert(w.fields == 1 && w.temperature == 18);
  assert(!bry::weatherCurrent(
      "seniverse",
      json(R"({"results":[{"now":{"temperature":"NaN","text":"Cloudy"}}]})"),
      w));
  assert(bry::weatherCurrent(
      "openweather",
      json(
          R"({"cod":200,"dt":1793880000,"name":"Hangzhou","main":{"temp":20,"feels_like":19,"humidity":45},"wind":{"speed":3},"weather":[{"description":"clear"}]})"),
      w));
  assert(bry::weatherForecast(
      "openweather",
      json(
          R"({"cod":"200","list":[{"dt":1793880000,"main":{"temp_min":13,"temp_max":21},"weather":[{"description":"clear"}]},{"dt":1793890800,"main":{"temp_min":12,"temp_max":24},"weather":[{"description":"clear"}]}]})"),
      w, 480));
  assert(w.days[0].low == 12 && w.days[0].high == 24);
  assert(!bry::weatherCurrent("openweather", json(R"({"cod":401})"), w));
  assert(!bry::weatherCurrent(
      "qweather",
      json(
          R"({"condition":{"text":"晴"},"temperature":{"value":80,"unit":"°C"}})"),
      w));
  char text[7];
  bry::textCopy(text, sizeof(text), "天气测试");
  assert(!strcmp(text, "天气"));
  std::string large = "{\"cod\":\"200\",\"unused\":\"" +
                      std::string(20000, 'x') + "\",\"list\":[";
  for (unsigned i = 0; i < 24; i++) {
    if (i)
      large += ",";
    large += "{\"dt\":" + std::to_string(1793880000u + i * 10800) +
             ",\"main\":{\"temp_min\":12,\"temp_max\":24},\"weather\":[{"
             "\"description\":\"clear\"}],\"unused\":\"" +
             std::string(100, 'z') + "\"}";
  }
  large += "]}";
  assert(bry::weatherForecast("openweather", json(large.c_str()), w, 480));
  assert(doc.memoryUsage() < 12288 && !doc.containsKey("unused") &&
         !doc["list"][0].containsKey("unused"));
  /* Actual keyless HTTPS response, Guangzhou, 2026-11-06. */
  std::ifstream file(WEATHER_FIXTURE_PATH);
  assert(file.good());
  std::string fixture((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  DynamicJsonDocument compact(4096);
  assert(bry::weatherDecode((const uint8_t *)fixture.data(), fixture.size(), compact));
  assert(bry::weatherCurrent("openmeteo", compact.as<JsonVariantConst>(), w));
  assert(bry::weatherForecast("openmeteo", compact.as<JsonVariantConst>(), w, 480));
  assert(bry::weatherCurrent("openmeteo", json(fixture.c_str()), w));
  assert(w.fields == 15 && w.observed == 1793959200u);
  assert(fabs(w.temperature - 26.1f) < .001 && w.humidity == 84 &&
         fabs(w.feels - 31.1f) < .001 && fabs(w.wind - 1.23f) < .001 &&
         !strcmp(w.text, "Moderate drizzle"));
  // Daily dates belong to the weather location, independent of screen TZ.
  assert(bry::weatherForecast("openmeteo", doc.as<JsonVariantConst>(), w, -300));
  assert(!strcmp(w.days[0].date, "2026-11-06") &&
         !strcmp(w.days[1].date, "2026-11-07") &&
         !strcmp(w.days[2].date, "2026-11-08") &&
         fabs(w.days[0].low - 22.5f) < .001 &&
         fabs(w.days[0].high - 30.4f) < .001);
  w.ttl = 1800000;
  strcpy(w.location, "Guangzhou, Guangdong");
  bry::weatherWire(w, p);
  assert(weather_receive(&received, p, sizeof(p), 700));
  assert(!strcmp(received.provider, "openmeteo") &&
         !strcmp(received.location, "Guangzhou, Guangdong") &&
         received.observed == w.observed && received.ttl == w.ttl &&
         received.days[2].valid && received.days[2].high == w.days[2].high);
  doc["current"]["apparent_temperature"] = nullptr;
  doc["current_units"]["wind_speed_10m"] = "km/h";
  assert(bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
  assert(w.fields == (1 | 4)); // No invented optional values or wrong units.
  doc["current"]["temperature_2m"] = nullptr;
  assert(!bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
  json(fixture.c_str());doc["current_units"]["temperature_2m"] = "°F";
  assert(!bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
  json(fixture.c_str());doc["current"]["weather_code"] = nullptr;
  assert(!bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
  doc["current"]["weather_code"] = 999;
  assert(!bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
  doc["current"]["weather_code"] = 1.5;
  assert(!bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
  for (int code : {0,1,2,3,45,48,51,53,55,56,57,61,63,65,66,67,71,73,75,
                   77,80,81,82,85,86,95,96,97,99}) {
    doc["current"]["weather_code"] = code;
    assert(bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
    for (const char *c = w.text; *c; c++) assert((unsigned char)*c < 128);
  }
  doc["current"]["time"] = 0;
  assert(!bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
  json(fixture.c_str());doc["current_units"]["time"] = "iso8601";
  assert(!bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
  json(fixture.c_str());doc["daily"]["temperature_2m_min"][0] = nullptr;
  assert(!bry::weatherForecast("openmeteo", doc.as<JsonVariantConst>(), w, 480));
  json(fixture.c_str());doc["daily"]["temperature_2m_max"].as<JsonArray>().remove(2);
  assert(!bry::weatherForecast("openmeteo", doc.as<JsonVariantConst>(), w, 480));
  json(fixture.c_str());doc["daily_units"]["temperature_2m_max"] = "°F";
  assert(!bry::weatherForecast("openmeteo", doc.as<JsonVariantConst>(), w, 480));
  json(fixture.c_str());doc["daily"]["time"][1] = doc["daily"]["time"][0];
  assert(!bry::weatherForecast("openmeteo", doc.as<JsonVariantConst>(), w, 480));
  json(fixture.c_str());doc["utc_offset_seconds"] = 86400;
  assert(!bry::weatherForecast("openmeteo", doc.as<JsonVariantConst>(), w, 480));
  json(fixture.c_str());doc["error"] = true;
  assert(!bry::weatherCurrent("openmeteo", doc.as<JsonVariantConst>(), w));
  assert(!bry::weatherForecast("openmeteo", doc.as<JsonVariantConst>(), w, 480));
  puts("weather: four schemas, actual Guangzhou keyless fixture, WMO codes, "
       "dates, null/units/errors, filtered forecast in 12 KiB and MCU wire");
}
