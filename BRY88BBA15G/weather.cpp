#include "weather.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
namespace bry {
bool weatherDecode(const uint8_t *body, size_t size, DynamicJsonDocument &doc) {
  /* A full OpenWeather forecast contains many fields unused by the display.
   * Keep the decoded document within 12 KiB on a non-PSRAM ESP32. */
  StaticJsonDocument<2048> filter;
  for (const char *key :
       {"error", "temperature", "feelsLike", "humidity", "wind", "condition",
        "cod", "main", "weather", "name", "dt", "utc_offset_seconds",
        "current_units", "current", "daily_units", "daily"})
    filter[key] = true;
  auto result = filter["results"][0];
  result["location"]["name"] = true;
  result["now"] = true;
  result["daily"] = true;
  auto day = filter["days"][0];
  for (const char *key :
       {"forecastStartTime", "temperatureMin", "temperatureMax"})
    day[key] = true;
  day["astro"]["solarNoon"] = true;
  day["daytime"]["condition"] = true;
  auto list = filter["list"][0];
  list["dt"] = true;
  list["main"] = true;
  list["weather"] = true;
  if (filter.overflowed())
    return false;
  doc.clear();
  return !deserializeJson(doc, body, size,
                          DeserializationOption::Filter(filter)) &&
         !doc.overflowed();
}
void textCopy(char *out, size_t cap, const char *s) {
  if (!s)
    s = "";
  size_t n = strlen(s);
  if (n >= cap)
    n = cap - 1;
  while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
    --n;
  memcpy(out, s, n);
  out[n] = 0;
  if (!scene_utf8(out, cap))
    out[0] = 0;
}
static bool numeric(JsonVariantConst v, float &out, float low, float high) {
  double n;
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    if (!s || !*s)
      return false;
    char *end;
    n = strtod(s, &end);
    if (*end)
      return false;
  } else if (v.is<float>() || v.is<int>() || v.is<unsigned>())
    n = v.as<double>();
  else
    return false;
  if (!std::isfinite(n) || n < low || n > high)
    return false;
  out = n;
  return true;
}
static void field(screen_weather &w, unsigned bit, float &value,
                  JsonVariantConst v, float low, float high, float scale = 1) {
  float n;
  if (numeric(v, n, low, high)) {
    value = n * scale;
    w.fields |= bit;
  }
}
static bool epochSeconds(JsonVariantConst v, uint32_t &epoch) {
  if (!v.is<uint32_t>() || v.as<uint32_t>() < 1700000000u)
    return false;
  epoch = v.as<uint32_t>();
  return true;
}
static const char *wmoText(JsonVariantConst code) {
  if (!code.is<int>())
    return "";
  /* ASCII fits the fonts already installed on community screens. WMO codes
   * follow Open-Meteo's documented interpretation, including code 97. */
  switch (code.as<int>()) {
  case 0: return "Clear sky";
  case 1: return "Mainly clear";
  case 2: return "Partly cloudy";
  case 3: return "Overcast";
  case 45: return "Fog";
  case 48: return "Rime fog";
  case 51: return "Light drizzle";
  case 53: return "Moderate drizzle";
  case 55: return "Dense drizzle";
  case 56: return "Light freezing drizzle";
  case 57: return "Dense freezing drizzle";
  case 61: return "Light rain";
  case 63: return "Moderate rain";
  case 65: return "Heavy rain";
  case 66: return "Light freezing rain";
  case 67: return "Heavy freezing rain";
  case 71: return "Light snow";
  case 73: return "Moderate snow";
  case 75: return "Heavy snow";
  case 77: return "Snow grains";
  case 80: return "Light rain showers";
  case 81: return "Moderate rain showers";
  case 82: return "Heavy rain showers";
  case 85: return "Light snow showers";
  case 86: return "Heavy snow showers";
  case 95: return "Thunderstorm";
  case 96: return "Thunderstorm with light hail";
  case 97: return "Heavy thunderstorm";
  case 99: return "Thunderstorm with heavy hail";
  default: return "";
  }
}
bool weatherCurrent(const char *provider, JsonVariantConst root,
                    screen_weather &out) {
  screen_weather w = {};
  textCopy(w.provider, sizeof(w.provider), provider);
  if (!strcmp(provider, "qweather")) {
    if (root["error"].is<JsonObjectConst>())
      return false;
    field(w, 1, w.temperature, root["temperature"]["value"], -100, 70);
    field(w, 2, w.feels, root["feelsLike"]["value"], -120, 100);
    field(w, 4, w.humidity, root["humidity"], 0, 1, 100);
    field(w, 8, w.wind, root["wind"]["speed"]["value"], 0, 150);
    if (strcmp(root["temperature"]["unit"] | "", "°C"))
      w.fields &= ~1u;
    if (strcmp(root["feelsLike"]["unit"] | "", "°C"))
      w.fields &= ~2u;
    if (strcmp(root["wind"]["speed"]["unit"] | "", "m/s"))
      w.fields &= ~8u;
    textCopy(w.text, sizeof(w.text), root["condition"]["text"] | "");
  } else if (!strcmp(provider, "seniverse")) {
    JsonVariantConst r = root["results"][0], n = r["now"];
    field(w, 1, w.temperature, n["temperature"], -100, 70);
    field(w, 2, w.feels, n["feels_like"], -120, 100);
    field(w, 4, w.humidity, n["humidity"], 0, 100);
    field(w, 8, w.wind, n["wind_speed"], 0, 540, 1.0f / 3.6f);
    textCopy(w.location, sizeof(w.location), r["location"]["name"] | "");
    textCopy(w.text, sizeof(w.text), n["text"] | "");
  } else if (!strcmp(provider, "openmeteo")) {
    JsonVariantConst n = root["current"], units = root["current_units"];
    if ((root["error"] | false) ||
        strcmp(units["time"] | "", "unixtime") ||
        strcmp(units["weather_code"] | "", "wmo code") ||
        strcmp(units["temperature_2m"] | "", "°C") ||
        !epochSeconds(n["time"], w.observed))
      return false;
    field(w, 1, w.temperature, n["temperature_2m"], -100, 70);
    if (!strcmp(units["apparent_temperature"] | "", "°C"))
      field(w, 2, w.feels, n["apparent_temperature"], -120, 100);
    if (!strcmp(units["relative_humidity_2m"] | "", "%"))
      field(w, 4, w.humidity, n["relative_humidity_2m"], 0, 100);
    if (!strcmp(units["wind_speed_10m"] | "", "m/s"))
      field(w, 8, w.wind, n["wind_speed_10m"], 0, 150);
    textCopy(w.text, sizeof(w.text), wmoText(n["weather_code"]));
  } else if (!strcmp(provider, "openweather")) {
    if (root["cod"].as<int>() != 200)
      return false;
    field(w, 1, w.temperature, root["main"]["temp"], -100, 70);
    field(w, 2, w.feels, root["main"]["feels_like"], -120, 100);
    field(w, 4, w.humidity, root["main"]["humidity"], 0, 100);
    field(w, 8, w.wind, root["wind"]["speed"], 0, 150);
    textCopy(w.location, sizeof(w.location), root["name"] | "");
    textCopy(w.text, sizeof(w.text), root["weather"][0]["description"] | "");
    w.observed = root["dt"] | 0u;
  } else
    return false;
  if (!(w.fields & 1) || !w.text[0])
    return false;
  w.valid = true;
  out = w;
  return true;
}
static bool day(weather_day &d, JsonVariantConst v, const char *date,
                const char *text, JsonVariantConst low, JsonVariantConst high) {
  (void)v;
  weather_day r = {};
  if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-')
    return false;
  if (!numeric(low, r.low, -100, 70) || !numeric(high, r.high, -100, 70) ||
      r.low > r.high)
    return false;
  textCopy(r.date, sizeof(r.date), date);
  textCopy(r.text, sizeof(r.text), text);
  r.valid = 1;
  d = r;
  return true;
}
bool weatherForecast(const char *provider, JsonVariantConst root,
                     screen_weather &w, int timezone) {
  unsigned count = 0;
  weather_day next[3] = {};
  if (!strcmp(provider, "seniverse")) {
    for (JsonVariantConst v :
         root["results"][0]["daily"].as<JsonArrayConst>()) {
      if (count == 3)
        break;
      if (day(next[count], v, v["date"] | "", v["text_day"] | "", v["low"],
              v["high"]))
        count++;
    }
  } else if (!strcmp(provider, "qweather")) {
    for (JsonVariantConst v : root["days"].as<JsonArrayConst>()) {
      if (count == 3)
        break;
      const char *date = v["astro"]["solarNoon"] | v["forecastStartTime"] | "";
      char localDate[11] = {};
      if (strlen(date) >= 10)
        memcpy(localDate, date, 10);
      if (!strcmp(v["temperatureMin"]["unit"] | "", "°C") &&
          !strcmp(v["temperatureMax"]["unit"] | "", "°C") &&
          day(next[count], v, localDate, v["daytime"]["condition"]["text"] | "",
              v["temperatureMin"]["value"], v["temperatureMax"]["value"]))
        count++;
    }
  } else if (!strcmp(provider, "openmeteo")) {
    JsonVariantConst units = root["daily_units"], d = root["daily"];
    if ((root["error"] | false) ||
        strcmp(units["time"] | "", "unixtime") ||
        strcmp(units["weather_code"] | "", "wmo code") ||
        strcmp(units["temperature_2m_min"] | "", "°C") ||
        strcmp(units["temperature_2m_max"] | "", "°C") ||
        !root["utc_offset_seconds"].is<int>())
      return false;
    int offset = root["utc_offset_seconds"];
    if (offset < -43200 || offset > 50400)
      return false;
    JsonArrayConst dates = d["time"].as<JsonArrayConst>(),
                   codes = d["weather_code"].as<JsonArrayConst>(),
                   lows = d["temperature_2m_min"].as<JsonArrayConst>(),
                   highs = d["temperature_2m_max"].as<JsonArrayConst>();
    size_t n = dates.size();
    if (!n || n > 3 || codes.size() != n || lows.size() != n || highs.size() != n)
      return false;
    for (size_t i = 0; i < n; i++) {
      uint32_t epoch;
      if (!epochSeconds(dates[i], epoch))
        return false;
      /* Unix timestamps are UTC. Daily dates use the API location's offset,
       * which can differ from the screen's configured display timezone. */
      time_t stamp = (int64_t)epoch + offset;
      struct tm local;
      char date[11];
      if (!gmtime_r(&stamp, &local) ||
          !strftime(date, sizeof(date), "%Y-%m-%d", &local))
        return false;
      const char *text = wmoText(codes[i]);
      if (!*text || (i && strcmp(next[i - 1].date, date) >= 0) ||
          !day(next[i], d, date, text, lows[i], highs[i]))
        return false;
      count++;
    }
  } else if (!strcmp(provider, "openweather")) {
    if (root["cod"].as<int>() != 200)
      return false;
    for (JsonVariantConst v : root["list"].as<JsonArrayConst>()) {
      uint32_t epoch = v["dt"] | 0u;
      if (epoch < 1700000000u)
        continue;
      time_t time = (int64_t)epoch + (int64_t)timezone * 60;
      struct tm tm;
      gmtime_r(&time, &tm);
      char date[11];
      strftime(date, sizeof(date), "%Y-%m-%d", &tm);
      float low, high;
      if (!numeric(v["main"]["temp_min"], low, -100, 70) ||
          !numeric(v["main"]["temp_max"], high, -100, 70) || low > high)
        continue;
      unsigned i = 0;
      while (i < count && strcmp(next[i].date, date))
        i++;
      if (i >= 3)
        break;
      if (i == count) {
        textCopy(next[i].date, sizeof(next[i].date), date);
        textCopy(next[i].text, sizeof(next[i].text),
                 v["weather"][0]["description"] | "");
        next[i].low = low;
        next[i].high = high;
        next[i].valid = 1;
        count++;
      } else {
        next[i].low = fminf(next[i].low, low);
        next[i].high = fmaxf(next[i].high, high);
      }
    }
  }
  if (!count)
    return false;
  memcpy(w.days, next, sizeof(next));
  return true;
}
static void floating(uint8_t *out, float n) {
  uint32_t bits;
  memcpy(&bits, &n, 4);
  scene_put32(out, bits);
}
void weatherWire(const screen_weather &w, uint8_t out[353]) {
  memset(out, 0, 353);
  out[0] = SC_MSG_WEATHER;
  out[1] = w.fields;
  scene_put32(out + 2, w.observed);
  scene_put32(out + 6, w.ttl / 1000);
  floating(out + 10, w.temperature);
  floating(out + 14, w.feels);
  floating(out + 18, w.humidity);
  floating(out + 22, w.wind);
  memcpy(out + 26, w.provider, 17);
  memcpy(out + 43, w.location, 49);
  memcpy(out + 92, w.text, 49);
  for (unsigned i = 0; i < 3; i++) {
    uint8_t *p = out + 141 + 70 * i;
    p[0] = w.days[i].valid;
    memcpy(p + 1, w.days[i].date, 11);
    memcpy(p + 12, w.days[i].text, 49);
    floating(p + 61, w.days[i].low);
    floating(p + 65, w.days[i].high);
  }
}
} // namespace bry
