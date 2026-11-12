#include <ArduinoJson.h>
#include <assert.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
using std::isfinite;
enum { WL_CONNECTED = 3, SNTP_SYNC_STATUS_RESET,
       SNTP_SYNC_STATUS_COMPLETED };
static struct {
  int state = 0;
  uint32_t address = 0;
  int status() { return state; }
  uint32_t localIP() { return address; }
} WiFi;
static unsigned starts;
static uint32_t ms;
static time_t epoch;
static int sync_status = SNTP_SYNC_STATUS_RESET;
static uint32_t millis() { return ms; }
static time_t clock_time(time_t *) { return epoch; }
static int sntp_get_sync_status() {
  int result = sync_status;
  sync_status = SNTP_SYNC_STATUS_RESET;
  return result;
}
static void configTime(long offset, int daylight, const char *a, const char *b) {
  assert(WiFi.state == WL_CONNECTED && WiFi.address);
  assert(!offset && !daylight && !strcmp(a, "pool.ntp.org") &&
         !strcmp(b, "time.cloudflare.com"));
  starts++;
}
#define time clock_time
#include "screen_clock.inc"
#undef time

int main() {
  StaticJsonDocument<1024> doc, input;
  auto status = [&]() {
    doc.clear();
    screenClockStatus(doc.to<JsonObject>(), 480);
    assert(!doc.overflowed());
  };
  screenClockPoll(0);
  status();
  assert(!starts && !doc["valid"].as<bool>() && doc["utc"].isNull());
  assert(!strcmp(doc["state"], "waiting_network"));
  WiFi.state = WL_CONNECTED;
  screenClockPoll(500);
  assert(!starts); // Association without a DHCP address is insufficient.
  WiFi.address = 0x01020304;
  screenClockPoll(1000);
  screenClockPoll(2000);
  assert(starts == 1);
  status();
  assert(!strcmp(doc["state"], "waiting_ntp"));
  assert(doc["sync_age_ms"].isNull() && doc["mcu_snapshot_utc"].isNull());
  // Retained ESP time is not an acknowledgement from an NTP server.
  epoch = 1793958000;
  status();
  assert(doc["valid"].as<bool>() && !doc["sntp_synced"].as<bool>());
  sync_status = SNTP_SYNC_STATUS_COMPLETED;
  screenClockPoll(UINT32_MAX - 999);
  ms = 1000;
  status();
  assert(doc["sntp_synced"].as<bool>());
  assert(doc["sync_age_ms"].as<uint32_t>() == 2000);
  assert(!strcmp(doc["state"], "synchronized"));
  screenClockPoll(ms);
  assert(starts == 1 && screenClockSynced); // Completion status is read once.
  input.set((double)epoch * 1000);
  screenClockSnapshot(input.as<JsonVariantConst>(), ms);
  ms += 2300;
  epoch += 2;
  status();
  assert(doc["mcu_offset_seconds"].as<int64_t>() == 0);
  assert(doc["mcu_snapshot_age_ms"].as<uint32_t>() == 2300);
  auto original = screenClockMcuUtc;
  const double invalid[] = {-1.0, 1699999999999.0, 4294967296000.0,
                            HUGE_VAL, NAN};
  for (double bad : invalid) {
    input.set(bad);
    screenClockSnapshot(input.as<JsonVariantConst>(), ms);
    assert(screenClockMcuUtc == original && screenClockMcuAt == 1000);
  }
  input.set("1793958000000");
  screenClockSnapshot(input.as<JsonVariantConst>(), ms);
  assert(screenClockMcuUtc == original);
  ms += 60000;
  status();
  assert(doc["mcu_offset_seconds"].isNull()); // No inferred stale MCU time.
  input.set(0);
  screenClockSnapshot(input.as<JsonVariantConst>(), ms);
  status();
  assert(doc["mcu_snapshot_utc"].isNull() &&
         doc["mcu_snapshot_age_ms"].isNull());
  WiFi.address++;
  screenClockPoll(ms);
  assert(starts == 2);
  WiFi.state = 0;
  screenClockPoll(ms);
  status();
  assert(doc["valid"].as<bool>() && !strcmp(doc["state"], "waiting_network"));
  WiFi.state = WL_CONNECTED;
  screenClockPoll(ms);
  assert(starts == 3); // Same IP still restarts after an observed disconnect.
  std::puts("Clock owner: DHCP gate, reconnect, real sync acknowledgement, "
            "UTC snapshot validation, stale samples and uptime wrap passed");
  return 0;
}
