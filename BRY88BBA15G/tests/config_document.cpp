#include "screen_config_document.h"
#include "screen_store_writer.h"
#include <cassert>
#include <cstdio>
#include <string>
static uint8_t flash[BRY_STORE_SIZE];
static size_t largestWrite;
static bool readFlash(void *, uint32_t at, void *out, size_t n) {
  assert(at+n <= sizeof(flash));
  std::memcpy(out, flash+at, n); return true;
}
static bool writeFlash(void *, uint32_t at, const void *data, size_t n) {
  assert(at+n <= sizeof(flash));
  if (n > largestWrite) largestWrite = n;
  for (size_t i = 0; i < n; i++) flash[at+i] &= static_cast<const uint8_t *>(data)[i];
  return true;
}
static bool eraseFlash(void *, uint32_t at, size_t n) {
  assert(at+n <= sizeof(flash) && !(at%4096) && !(n%4096));
  std::memset(flash+at, 255, n); return true;
}
int main() {
  std::memset(flash, 255, sizeof(flash));
  bry_store store = {};
  store.read = readFlash; store.write = writeFlash; store.erase = eraseFlash;
  assert(bry_store_probe(&store));
  reminder_schedule schedule = {};
  reminder_notice notices[REMINDER_MAX] = {};
  schedule.timezone = 480;
  StaticJsonDocument<4096> weather;
  StaticJsonDocument<1024> settings;
  weather["provider"] = "openmeteo";
  weather["label"] = "Guangzhou, Guangdong";
  weather["enabled"] = true;
  weather["key"] = std::string(256, 'k');
  settings["display"]["luminance"] = 0;
  settings["display"]["data_type"] = 0;
  settings["display"]["power_screen_time"] = 2;
  settings["display"]["battery_screen_time"] = 1;
  std::string layout(12696, 'A'),policy(492,'B'),appearance(940,'C');
  for (unsigned count : {0u, 1u, 32u}) {
    for (unsigned i = 0; i < REMINDER_MAX; i++) {
      auto &r = schedule.rules[i];
      r.id = i < count ? i + 1 : 0;
      r.kind = REM_WEEKLY;
      r.days = 31;
      r.minute = 570;
      r.enabled = true;
      r.last_fire = 1234;
      std::memset(r.title, 'T', sizeof(r.title) - 1);
      std::memset(r.text, 'R', sizeof(r.text) - 1);
      auto &n = notices[i];
      n.id = i < count ? 0x80000000u + i : 0;
      n.at = 5678;
      n.unread = true;
      std::memset(n.title, 'N', sizeof(n.title) - 1);
      std::memset(n.text, 'M', sizeof(n.text) - 1);
    }
    size_t capacity = screenConfigCapacity(weather, settings, schedule, notices);
    DynamicJsonDocument doc(capacity);
    assert(screenConfigDocument(doc, layout.c_str(), weather, settings,
                                 schedule, notices, policy.c_str(), appearance.c_str()));
    assert(doc["rules"].size() == count && doc["notices"].size() == count);
    assert(doc["settings"]["display"]["luminance"] == 0);
    assert(doc["weather"]["key"].as<std::string>() == std::string(256, 'k'));
    std::string wire;
    assert(serializeMsgPack(doc, wire) == measureMsgPack(doc));
    assert(wire.size() <= 48 * 1024);
    assert(screenPersistConfig(&store, doc));
    size_t persistedSize = 0;
    assert(bry_store_load(&store, nullptr, 0, &persistedSize));
    assert(persistedSize == wire.size());
    std::string persisted(persistedSize, '\0');
    assert(bry_store_load(&store, &persisted[0], persisted.size(), &persistedSize));
    assert(persisted == wire && largestWrite <= 256);
    DynamicJsonDocument restored(65536);
    assert(!deserializeMsgPack(restored, wire));
    assert(restored["layout"].as<std::string>() == layout);
    assert(restored["policy"].as<std::string>() == policy);
    assert(restored["appearance"].as<std::string>() == appearance);
    bool automatic=false;
    assert(screenConfigStartupAuto(restored.as<JsonObjectConst>(),automatic)&&automatic);
    doc["startup_auto_resume"]=false;
    assert(screenPersistConfig(&store,doc));
    assert(bry_store_load(&store,nullptr,0,&persistedSize));persisted.resize(persistedSize);
    assert(bry_store_load(&store,&persisted[0],persisted.size(),&persistedSize));
    restored.clear();assert(!deserializeMsgPack(restored,persisted));
    assert(screenConfigStartupAuto(restored.as<JsonObjectConst>(),automatic)&&!automatic);
    restored.remove("startup_auto_resume");
    assert(screenConfigStartupAuto(restored.as<JsonObjectConst>(),automatic)&&automatic);
    restored["startup_auto_resume"]="true";
    assert(!screenConfigStartupAuto(restored.as<JsonObjectConst>(),automatic)&&!automatic);
    assert(restored["rules"].size() == count && restored["notices"].size() == count);
    if (count) {
      assert(restored["rules"][count - 1]["last_fire"] == 1234);
      assert(restored["notices"][count - 1]["at"] == 5678);
      assert(restored["rules"][0]["text"].as<std::string>().size() == sizeof(schedule.rules[0].text) - 1);
    }
    if (!count) assert(capacity < 4096);
    assert(capacity < 40948);
    StaticJsonDocument<1> unavailable;
    assert(!screenConfigDocument(unavailable, layout.c_str(), weather, settings,
                                  schedule, notices, policy.c_str(), appearance.c_str()));
    std::printf("PASS real config composer: %u records each, capacity=%zu, "
                "binary bytes=%zu, complete restore\n", count, capacity, wire.size());
  }
}
