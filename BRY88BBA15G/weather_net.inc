/* Fetching is isolated from the sole screen UART owner. TLS is mandatory. */
#include "weather_ca.h"
#include "weather_openmeteo_ca.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp32/rom/miniz.h>
#include "weather_retry.h"
struct WeatherJob {
  char provider[17], host[128], key[257], label[49];
  float latitude, longitude;
  uint32_t generation, interval;
  int timezone;
  bool verifyTls, english;
};
struct WeatherResult {
  screen_weather value;
  uint32_t generation, stackFreeBytes, startHeap, startBlock, endHeap;
  bool valid;
  char error[32];
  char stage[24];
  int httpStatus, tlsError;
  uint32_t verifyFlags;
};
static QueueHandle_t screenWeatherResults = nullptr;
static bool screenWeatherBusy = false, screenWeatherRefresh = false;
static WeatherRetry screenWeatherRetry;
static uint32_t screenWeatherAttempts=0,screenWeatherStartHeap=0,screenWeatherStartBlock=0,screenWeatherEndHeap=0;
static uint32_t screenWeatherGeneration = 1,
                screenWeatherSentSession = 0, screenWeatherSentAt = 0,
                screenWeatherStackFree = 0;
static const char *screenWeatherError = "not_configured";
static char screenWeatherStage[24] = "idle";
static int screenWeatherHttpStatus = 0, screenWeatherTlsError = 0;
static uint32_t screenWeatherVerifyFlags = 0;
class WeatherTLS : public WiFiClientSecure {
public:
  uint32_t verifyFlags = 0;
  void stop() override {
    // WiFiClientSecure tears down a failed handshake before returning to HTTP.
    // Preserve diagnostics before the failed TLS session is released.
    if (_lastError < 0 && !verifyFlags) {
      uint32_t flags = mbedtls_ssl_get_verify_result(&sslclient->ssl_ctx);
      if (flags != UINT32_MAX) verifyFlags = flags;
    }
    WiFiClientSecure::stop();
  }
};
class WeatherBuffer : public Stream {
public:
  uint8_t *p;
  size_t used = 0, cap;
  bool overflow = false;
  WeatherBuffer(uint8_t *p, size_t n) : p(p), cap(n) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t n) override {
    if (n > cap - used) {
      overflow = true;
      return 0;
    }
    memcpy(p + used, b, n);
    used += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};
static bool weatherInflate(const uint8_t *p, size_t n, uint8_t *out, size_t cap,
                           size_t &used) {
  if (n < 18 || p[0] != 31 || p[1] != 139 || p[2] != 8 || (p[3] & 0xe0))
    return false;
  size_t at = 10;
  if (p[3] & 4) {
    if (at + 2 > n - 8)
      return false;
    size_t extra = p[at] | p[at + 1] << 8;
    at += 2;
    if (extra > n - 8 - at)
      return false;
    at += extra;
  }
  for (unsigned bit : {8u, 16u})
    if (p[3] & bit) {
      while (at < n - 8 && p[at])
        at++;
      if (at >= n - 8)
        return false;
      at++;
    }
  if (p[3] & 2) {
    if (at + 2 > n - 8)
      return false;
    at += 2;
  }
  if (at >= n - 8)
    return false;
  // The ROM mem-to-mem helper puts the >11 KiB decompressor on the caller's
  // stack. This task has 8 KiB: own that state on the heap instead.
  std::unique_ptr<tinfl_decompressor> state(new (std::nothrow) tinfl_decompressor{});
  if (!state)
    return false;
  tinfl_init(state.get());
  size_t input = n - 8 - at;
  used = cap;
  auto status = tinfl_decompress(state.get(), p + at, &input, out, out, &used,
                                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return status == TINFL_STATUS_DONE && input == n - 8 - at &&
         used == scene_u32(p + n - 4) &&
         scene_crc32(out, used) == scene_u32(p + n - 8);
}
static bool weatherGet(const String &url, const WeatherJob &job,
                       DynamicJsonDocument &doc, WeatherResult &result) {
  const size_t bodyLimit = !strcmp(job.provider, "openmeteo") ? 4096 : 16384;
  // Release the previous forecast before connecting. Reserve the bounded body
  // first: a successful TLS handshake can leave no contiguous response block,
  // especially while the web editor is polling. Never start a request whose
  // response we cannot receive. Gzip output and JSON remain deferred until
  // after the TLS session has released its buffers.
  doc = DynamicJsonDocument(0);
  strcpy(result.stage, "response_allocation");
  std::unique_ptr<uint8_t[]> raw(new (std::nothrow) uint8_t[bodyLimit]);
  if (!raw)
    return false;
  WeatherTLS tls;
  // Use mbedTLS's chain builder for this cross-signed ISRG chain. The Arduino
  // bundle callback can reject it before reaching its existing X1/X2 anchor.
  // Verified mode keeps the hostname/time checks and uses two PEM roots.
  if (!job.verifyTls) tls.setInsecure();
  else if (!strcmp(job.provider, "openmeteo")) tls.setCACert(weatherOpenMeteoCa);
  else tls.setCACertBundle(weatherCaBundle);
  tls.setTimeout(6000);
  tls.setHandshakeTimeout(6); // Arduino defaults to 120 seconds.
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  strcpy(result.stage, "request_begin");
  if (!http.begin(tls, url))
    return false;
  const char *names[] = {"Content-Encoding"};
  http.collectHeaders(names, 1);
  http.addHeader("Accept-Encoding", "gzip");
  if (!strcmp(job.provider, "qweather"))
    http.addHeader("X-QW-Api-Key", job.key);
  strcpy(result.stage, "http_request");
  int code = http.GET();
  result.httpStatus = code;
  if (code < 0) {
    char tlsMessage[1];
    result.tlsError = tls.lastError(tlsMessage, sizeof(tlsMessage));
    result.verifyFlags = tls.verifyFlags;
  }
  if (code != 200 || http.getSize() > (int)bodyLimit) {
    if (code == 200) strcpy(result.stage, "response_size");
    http.end();
    return false;
  }
  WeatherBuffer stream(raw.get(), bodyLimit);
  int received = http.writeToStream(&stream);
  String encoding = http.header("Content-Encoding");
  http.end();
  tls.stop(); // Release TLS buffers before allocating the gzip output.
  strcpy(result.stage, "response_body");
  if (received < 0 || stream.overflow || !stream.used)
    return false;
  const uint8_t *body = raw.get();
  size_t size = stream.used;
  std::unique_ptr<uint8_t[]> inflated;
  if (encoding == "gzip") {
    strcpy(result.stage, "response_gzip");
    size_t decodedLimit = !strcmp(job.provider, "openmeteo") ? 4096 : 32768;
    inflated.reset(new (std::nothrow) uint8_t[decodedLimit]);
    if (!inflated || !weatherInflate(body, size, inflated.get(), decodedLimit, size))
      return false;
    body = inflated.get();
  } else if (encoding.length() && encoding != "identity") {
    strcpy(result.stage, "response_encoding");
    return false;
  }
  strcpy(result.stage, "response_json");
  doc = DynamicJsonDocument(!strcmp(job.provider, "openmeteo") ? 4096 : 12288);
  return bry::weatherDecode(body, size, doc);
}
static void screenWeatherWorker(void *argument) {
  { /* Free C++ request buffers before the FreeRTOS task deletes itself. */
    std::unique_ptr<WeatherJob> job((WeatherJob *)argument);
    WeatherResult result = {};
    result.generation = job->generation;
    result.startHeap=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    result.startBlock=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    String current, forecast;
    const String language=job->english?"en":"zh-Hans";
    char coordinates[64];
    snprintf(coordinates, sizeof(coordinates), "%.2f/%.2f",
             (double)job->latitude, (double)job->longitude);
    if (!strcmp(job->provider, "qweather")) {
      String base = "https://" + String(job->host) + "/weather/v1/";
      current = base + "current/" + coordinates + "?lang="+language;
      forecast =
          base + "daily/" + coordinates + "?days=3&localTime=true&lang="+language;
    } else if (!strcmp(job->provider, "seniverse")) {
      snprintf(coordinates, sizeof(coordinates), "%.4f:%.4f",
               (double)job->latitude, (double)job->longitude);
      String base = "https://api.seniverse.com/v3/weather/",
             query = "?key=" + String(job->key) + "&location=" + coordinates +
                     "&language="+language+"&unit=c";
      current = base + "now.json" + query;
      forecast = base + "daily.json" + query + "&start=0&days=3";
    } else if (!strcmp(job->provider, "openmeteo")) {
      snprintf(coordinates, sizeof(coordinates), "?latitude=%.4f&longitude=%.4f",
               (double)job->latitude, (double)job->longitude);
      current = String("https://api.open-meteo.com/v1/forecast") + coordinates +
                "&current=temperature_2m,relative_humidity_2m,"
                "apparent_temperature,weather_code,wind_speed_10m"
                "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                "&temperature_unit=celsius&wind_speed_unit=ms&timezone=auto"
                "&forecast_days=3&timeformat=unixtime";
      // Current and forecast share this one small response; no second fetch.
    } else {
      snprintf(coordinates, sizeof(coordinates), "?lat=%.4f&lon=%.4f",
               (double)job->latitude, (double)job->longitude);
      String base = "https://api.openweathermap.org/data/2.5/",
             query = String(coordinates) + "&appid=" + job->key +
                     String("&units=metric&lang=")+(job->english?"en":"zh_cn");
      current = base + "weather" + query;
      forecast = base + "forecast" + query + "&cnt=24";
    }
    DynamicJsonDocument doc(0);
    bool fetched = weatherGet(current, *job, doc, result);
    if (fetched) strcpy(result.stage, "current_schema");
    if (fetched && bry::weatherCurrent(job->provider, doc.as<JsonVariantConst>(),
                                     result.value)) {
      result.valid = true;
      if (!result.value.observed)
        result.value.observed = time(nullptr);
      if (job->label[0])
        bry::textCopy(result.value.location, sizeof(result.value.location),
                      job->label);
      result.value.ttl = min(job->interval * 2, 86400u) * 1000;
      bool forecastFetched = !strcmp(job->provider, "openmeteo") ||
                             weatherGet(forecast, *job, doc, result);
      if (forecastFetched) strcpy(result.stage, "forecast_schema");
      if (!forecastFetched ||
          !bry::weatherForecast(job->provider, doc.as<JsonVariantConst>(),
                               result.value, job->timezone))
        strcpy(result.error, "forecast_unavailable");
      else strcpy(result.stage, "complete");
    } else
      strcpy(result.error, "request_or_response_failed");
    result.stackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
    result.endHeap=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(result.tlsError == -0x7f00)strcpy(result.error,"tls_allocation_failed");
    xQueueSend(screenWeatherResults, &result, 0);
  }
  vTaskDelete(nullptr);
}
static void screenWeatherPoll(uint32_t now) {
  if (!screenWeatherResults)
    screenWeatherResults = xQueueCreate(1, sizeof(WeatherResult));
  WeatherResult result;
  if (screenWeatherResults &&
      xQueueReceive(screenWeatherResults, &result, 0) == pdTRUE) {
    screenWeatherBusy = false;
    if (result.generation == screenWeatherGeneration) {
      bry::textCopy(screenWeatherStage, sizeof(screenWeatherStage), result.stage);
      screenWeatherHttpStatus = result.httpStatus;
      screenWeatherTlsError = result.tlsError;
      screenWeatherVerifyFlags = result.verifyFlags;
      screenWeatherStackFree = result.stackFreeBytes;
      screenWeatherStartHeap=result.startHeap;screenWeatherStartBlock=result.startBlock;screenWeatherEndHeap=result.endHeap;
      if(result.valid&&!result.error[0])screenWeatherRetry.success(now,screenWeatherConfig["interval"]|900u);
      else screenWeatherRetry.failed(now);
      if (result.valid) {
        screenWeather = result.value;
        screenWeather.at = now;
        screenWeatherSentAt = 0;
        screenWeatherSentSession = 0;
      }
      screenWeatherError = result.error[0]
                               ? (result.valid ? "forecast_unavailable"
                                               : (result.tlsError==-0x7f00?"tls_allocation_failed":"request_or_response_failed"))
                               : "";
      invalidateBusinessStatusCache();
    }
  }
  if (screenWeather.valid && now - screenWeather.at >= screenWeather.ttl)
    screenWeather.expired = true;
  if (screenWeather.valid && !screenWeather.expired &&
      (screenBridge.capabilities & SCREEN_CAP_SERVICES) &&
      (screenWeatherSentSession != screenBridge.session ||
       now - screenWeatherSentAt >= 60000)) {
    screen_weather outgoing = screenWeather;
    outgoing.ttl -=
        now - screenWeather.at; // Replays must retain the original expiry.
    uint8_t p[353];
    bry::weatherWire(outgoing, p);
    if (outgoing.ttl >= 1000 &&
        bry_bridge_send(&screenBridge, p, sizeof(p), SCREEN_CAP_SERVICES)) {
      screenWeatherSentSession = screenBridge.session;
      screenWeatherSentAt = now;
    }
  }
  if (g3OnlineBusy || screenWeatherBusy || screenPowerWanted.load() || screenImage.active || screenLayoutUpload.active() ||
      screenFontBusy() || screenWallpaper.active || screenWallpaper.pending ||
      !(screenWeatherConfig["enabled"] | false) ||
      WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000)
    return;
  if (!screenWeatherRetry.due(now,screenWeatherRefresh)) return;
  const size_t byteHeap=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
      byteBlock=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if (!screenWeatherResults || !WeatherRetry::memoryReady(byteHeap,byteBlock,
        !strcmp(screenWeatherConfig["provider"]|"","openmeteo"))) {
    screenWeatherError="insufficient_byte_heap";
    strcpy(screenWeatherStage,"heap_wait");
    screenWeatherStartHeap=byteHeap;screenWeatherStartBlock=byteBlock;
    screenWeatherRetry.failed(now);screenWeatherRefresh=false;
    return;
  }
  auto *job = new (std::nothrow) WeatherJob{};
  if (!job) {screenWeatherRetry.failed(now);screenWeatherRefresh=false;return;}
  bry::textCopy(job->provider, sizeof(job->provider),
                screenWeatherConfig["provider"] | "");
  bry::textCopy(job->host, sizeof(job->host), screenWeatherConfig["host"] | "");
  bry::textCopy(job->key, sizeof(job->key), screenWeatherConfig["key"] | "");
  bry::textCopy(job->label, sizeof(job->label),
                screenWeatherConfig["label"] | "");
  job->latitude = screenWeatherConfig["latitude"] | 0.0f;
  job->longitude = screenWeatherConfig["longitude"] | 0.0f;
  job->generation = screenWeatherGeneration;
  job->interval = screenWeatherConfig["interval"] | 900u;
  job->timezone = screenSchedule.timezone;
  job->english=screenAppearance.language==1;
  job->verifyTls = screenWeatherConfig["verify_tls"] | false;
  screenWeatherRetry.begin(now);++screenWeatherAttempts;
  screenWeatherRefresh = false;
  screenWeatherBusy = true;
  strcpy(screenWeatherStage, "request_queued");
  screenWeatherHttpStatus = screenWeatherTlsError = 0;
  screenWeatherVerifyFlags = 0;
  if (xTaskCreate(screenWeatherWorker, "weather", 8192, job, 1, nullptr) !=
      pdPASS) {
    delete job;
    screenWeatherBusy = false;
    screenWeatherError = "task_allocation_failed";
    screenWeatherRetry.failed(now);
  }
}

static void screenWeatherLanguageChanged(){++screenWeatherGeneration;screenWeatherRefresh=true;screenWeatherSentAt=0;}
