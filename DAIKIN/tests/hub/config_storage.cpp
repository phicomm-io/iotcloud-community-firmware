// SPDX-License-Identifier: GPL-3.0-or-later
// Production config/Preferences methods and IDF NVS; only Flash I/O is modeled.
#include "nvs_storage.hpp"
#include "hub_service.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using String = std::string;
extern "C" void esp_log_write(int, const char *, const char *, ...) {}
extern "C" uint32_t esp_log_timestamp() { return 0; }
extern "C" void _esp_error_check_failed(esp_err_t code, const char *file, int line,
                                        const char *function, const char *expression) {
  fprintf(stderr, "NVS assertion %x %s:%d %s %s\n", code, file, line, function, expression);
  abort();
}
struct PowerLoss {};
class Nor : public nvs::Partition {
public:
  std::vector<uint8_t> bytes = std::vector<uint8_t>(16384, 255);
  long cut = -1;
  bool fail = false;
  size_t work = 0;
  const char *get_partition_name() override { return "nvs"; }
  uint32_t get_address() override { return 0; }
  uint32_t get_size() override { return bytes.size(); }
  esp_err_t read_raw(size_t offset, void *out, size_t size) override { return read(offset, out, size); }
  esp_err_t read(size_t offset, void *out, size_t size) override {
    if (offset + size > bytes.size()) return ESP_ERR_INVALID_ARG;
    memcpy(out, bytes.data() + offset, size); return ESP_OK;
  }
  esp_err_t write_raw(size_t offset, const void *in, size_t size) override { return write(offset, in, size); }
  esp_err_t write(size_t offset, const void *in, size_t size) override {
    if (fail) return ESP_ERR_FLASH_OP_FAIL;
    if (offset + size > bytes.size()) return ESP_ERR_INVALID_ARG;
    auto data = static_cast<const uint8_t *>(in);
    for (size_t i = 0; i < size; i++) {
      if (cut == 0) throw PowerLoss();
      if (cut > 0) --cut;
      assert((uint8_t(~bytes[offset + i]) & data[i]) == 0);
      bytes[offset + i] &= data[i]; ++work;
    }
    return ESP_OK;
  }
  esp_err_t erase_range(size_t offset, size_t size) override {
    if (fail) return ESP_ERR_FLASH_OP_FAIL;
    if (offset + size > bytes.size() || offset % 4096 || size % 4096) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < size; i++) {
      if (cut == 0) throw PowerLoss();
      if (cut > 0) --cut;
      bytes[offset + i] = 255; ++work;
    }
    return ESP_OK;
  }
};
static std::unique_ptr<nvs::Storage> storage;
static uint8_t space;
static Nor *flash;
static void boot(Nor &nor) {
  storage.reset(); flash = &nor;
  storage.reset(new(std::nothrow) nvs::Storage(&nor)); assert(storage);
  assert(storage->init(0, 4) == ESP_OK);
  assert(storage->createOrOpenNamespace("daikin", true, space) == ESP_OK);
}
static esp_err_t get(const char *key, nvs::ItemType type, void *out, size_t *length) {
  size_t required = 0;
  auto result = storage->getItemDataSize(space, type, key, required);
  if (result != ESP_OK) return result;
  if (!out) { *length = required; return ESP_OK; }
  if (*length < required) return ESP_ERR_NVS_INVALID_LENGTH;
  *length = required; return storage->readItem(space, type, key, out, required);
}
extern "C" esp_err_t nvs_get_str(nvs_handle_t, const char *key, char *out, size_t *length) { return get(key, nvs::ItemType::SZ, out, length); }
extern "C" esp_err_t nvs_get_blob(nvs_handle_t, const char *key, void *out, size_t *length) { return get(key, nvs::ItemType::BLOB, out, length); }
extern "C" esp_err_t nvs_set_str(nvs_handle_t, const char *key, const char *value) { return storage->writeItem(space, nvs::ItemType::SZ, key, value, strlen(value) + 1); }
extern "C" esp_err_t nvs_set_blob(nvs_handle_t, const char *key, const void *value, size_t length) { return storage->writeItem(space, nvs::ItemType::BLOB, key, value, length); }
// IDF 4.4.7 NVSHandleSimple::commit() is a validity check, with no deferred writes.
extern "C" esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
#define NUMBER(name, type) extern "C" esp_err_t nvs_get_##name(nvs_handle_t, const char *key, type *out) { return storage->readItem(space, key, *out); }
NUMBER(i8, int8_t) NUMBER(u8, uint8_t) NUMBER(i16, int16_t) NUMBER(u16, uint16_t)
NUMBER(i32, int32_t) NUMBER(u32, uint32_t) NUMBER(i64, int64_t) NUMBER(u64, uint64_t)
#undef NUMBER
#define log_e(...) ((void)0)
enum PreferenceType { PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64, PT_STR, PT_BLOB, PT_INVALID };
class Preferences {
  bool _started = true, _readOnly = false;
  uint32_t _handle = 1;
public:
  size_t putString(const char *, const char *);
  size_t putString(const char *, String);
  size_t putBytes(const char *, const void *, size_t);
  PreferenceType getType(const char *);
  bool isKey(const char *);
  String getString(const char *, String);
  size_t getBytesLength(const char *);
  size_t getBytes(const char *, void *, size_t);
};
#include "preferences.inc"
static Preferences prefs;
#ifdef IOTCLOUD_HUB
static const size_t configCapacity = 16384;
#else
static const size_t configCapacity = 6144;
#endif
static DynamicJsonDocument config(configCapacity);
static bool configRecovery = false, restarting = false;
static String configText;
static unsigned passwordUpdates = 0;
static bool hubRestartPending() { return restarting; }
static void updatePassword() { ++passwordUpdates; }
#include "load.inc"
// The Arduino String writer may accept only a prefix when realloc fails. Keep
// its optimistic byte count, but inject that partial destination into save.
static bool shortWriter = false;
static size_t storageSerializeJson(const JsonDocument &document, String &text) {
  const auto count = serializeJson(document, text);
  if (shortWriter) text.resize(text.size() / 2);
  return count;
}
#define serializeJson storageSerializeJson
#include "save.inc"
#undef serializeJson
static std::string legacy() {
  std::string text = R"({"_version":1,"wifi":{"ssid":"lab","password":"test-password"},"mqtt":{"host":"broker.local","port":1883,"user":"lab","password":"test-upstream"},"auth":{"password":"test-web-password"}})";
  text.resize(479, ' '); return text;
}
static void seed(Nor &nor) {
  boot(nor); auto old = legacy();
  assert(prefs.putString("config", old.c_str()) == old.size());
  uint8_t wifi; assert(storage->createOrOpenNamespace("nvs.net80211", true, wifi) == ESP_OK);
  const std::string value(512, 'w');
  assert(storage->writeItem(wifi, nvs::ItemType::BLOB, "wifi-state", value.data(), value.size()) == ESP_OK);
}
static void wifiIntact() {
  uint8_t wifi; assert(storage->createOrOpenNamespace("nvs.net80211", false, wifi) == ESP_OK);
  std::string value(512, 0);
  assert(storage->readItem(wifi, nvs::ItemType::BLOB, "wifi-state", &value[0], value.size()) == ESP_OK);
  assert(value == std::string(512, 'w'));
}
static void intact() {
  assert(prefs.getString("config", "invalid") == legacy()); wifiIntact();
}
static void load() {
  config = DynamicJsonDocument(configCapacity); configText = loadConfigText();
  assert(!deserializeJson(config, configText)); config.shrinkToFit();
  assert(config.capacity() >= config.memoryUsage());
  assert(config.capacity() - config.memoryUsage() < sizeof(void *));
}
static std::string snapshot() { std::string text; serializeJson(config, text); return text; }
static void migrated(const std::string &marker) {
  DynamicJsonDocument next(16384); assert(next.set(config));
  next["device"][std::string("name")] = marker;
  assert(saveConfig(next)); assert(next.capacity() == 0);
  assert(config["device"]["name"] == marker);
  assert(config.capacity() < 16384);
}
static void collision() {
  Nor nor; seed(nor);
  bool stale = false;
  for (unsigned i = 0; i < 12; i++) {
    const std::string blob(864 + 94 * i, char('a' + i));
    assert(prefs.putBytes("config", blob.data(), blob.size()) == blob.size());
    assert(prefs.getType("config") == PT_STR);
    auto size = prefs.getBytesLength("config");
    // A stale length is sufficient evidence. Reading the colliding blob can
    // assert inside IDF itself when its stale index and data chunks disagree.
    stale |= size != blob.size();
  }
  boot(nor); assert(prefs.getType("config") == PT_STR); intact();
  assert(stale && loadConfigText() == legacy());
  printf("IDF same-key STR/BLOB collision reproduced; old STR wins after reboot\n");
}
static void migration() {
  Nor nor; seed(nor);
  for (unsigned i = 0; i < 3; i++) { std::string junk(864 + 94 * i, 'x'); assert(prefs.putBytes("config", junk.data(), junk.size()) == junk.size()); }
  load(); migrated(std::string(63, 'm')); auto expected = snapshot(); intact();
  boot(nor); assert(loadConfigText() == expected); load(); intact();
  assert(config["device"]["name"] == std::string(63, 'm'));
  printf("new key commits and reboots beside legacy collision debris, legacy/WiFi intact\n");
}
static void invalid() {
  for (const auto &blob : {std::string(), std::string("{"), std::string(6145, 'x')}) {
    Nor nor; seed(nor); assert(storage->writeItem(space, nvs::ItemType::BLOB, "config_hub", blob.data(), blob.size()) == ESP_OK);
    assert(prefs.isKey("config_hub")); auto text = loadConfigText();
    assert(text != legacy()); DynamicJsonDocument rejected(16384); assert(deserializeJson(rejected, text)); intact();
  }
  Nor nor; seed(nor); uint8_t wrong = 1;
  assert(storage->writeItem(space, "config_hub", wrong) == ESP_OK);
  assert(loadConfigText() == "invalid"); intact();
}
static void powerCase(bool update) {
  Nor initial; seed(initial); load();
  if (update) migrated("previous-owner-generation");
  const auto old = loadConfigText();
  DynamicJsonDocument next(16384); assert(next.set(config));
  next["device"][std::string("name")] = std::string(63, 'n');
  std::string wanted; serializeJson(next, wanted);
  initial.work = 0; assert(saveConfig(next)); size_t work = initial.work;
  // Recreate the exact pre-update Flash image, then cut every byte. Exceptions
  // stop immediately, without post-failure cleanup, as a real reset would.
  Nor before; seed(before); load(); if (update) migrated("previous-owner-generation");
  size_t cases = 0;
  for (size_t cut = 0; cut <= work; ++cut) {
    Nor nor; nor.bytes = before.bytes; boot(nor); load();
    DynamicJsonDocument candidate(16384); assert(candidate.set(config));
    candidate["device"][std::string("name")] = std::string(63, 'n');
    nor.cut = cut;
    try { saveConfig(candidate); } catch (const PowerLoss &) {}
    nor.cut = -1; boot(nor);
    auto actual = loadConfigText(); assert(actual == old || actual == wanted);
    intact(); ++cases;
  }
  printf("power-cut %s cases=%zu NOR-bytes=%zu\n", update ? "update" : "first commit", cases, work);
}
static void failure() {
  Nor nor; seed(nor); load(); migrated("committed");
  auto old = snapshot(); unsigned updates = passwordUpdates;
  DynamicJsonDocument candidate(16384); assert(candidate.set(config));
  candidate["device"][std::string("name")] = "rejected";
  nor.fail = true; assert(!saveConfig(candidate)); nor.fail = false;
  assert(snapshot() == old && passwordUpdates == updates && candidate.capacity() == 16384);
  boot(nor); assert(loadConfigText() == old); intact();
  load(); DynamicJsonDocument oversized(16384); assert(oversized.set(config));
  oversized["padding"] = std::string(6144, 'x'); assert(!saveConfig(oversized));
  assert(snapshot() == old && loadConfigText() == old); intact();
  restarting = true; assert(!saveConfig(candidate)); restarting = false;
}
static void shortWrite() {
  Nor nor; seed(nor); load();
#ifdef IOTCLOUD_HUB
  migrated("committed");
#endif
  const auto old = snapshot(); const auto durable = loadConfigText();
  const auto updates = passwordUpdates;
  DynamicJsonDocument candidate(configCapacity); assert(candidate.set(config));
  candidate["device"][std::string("name")] = "partial-must-not-commit";
  nor.work = 0; shortWriter = true;
  assert(!saveConfig(candidate)); shortWriter = false;
  assert(nor.work == 0 && snapshot() == old && passwordUpdates == updates);
  assert(candidate.capacity() == configCapacity && loadConfigText() == durable);
  boot(nor); assert(loadConfigText() == durable); wifiIntact();
  // Adding _version must also fit; a failed insertion must not persist a
  // document that passed the earlier overflow check.
  DynamicJsonDocument full(0); assert(!full.overflowed());
  nor.work = 0; assert(!saveConfig(full)); assert(full.overflowed());
  assert(nor.work == 0 && snapshot() == old && passwordUpdates == updates);
}
static void budget() {
  Nor nor; seed(nor); load(); iotcloud::HubService service(config);
  service.store = saveConfig;
  service.randomHex = [](size_t n) { return std::string(n * 2, 'a'); };
  service.derive = [](const std::string &, const std::string &) { return std::string(64, 'b'); };
  using R = iotcloud::HubService::Request;
  const std::string credentials = R"({"username":"local","password":"local-password"})";
  assert(service.handle(R("POST", "/api/hub/setup", credentials, "", "", true), 0).status == 201);
  auto login = service.handle(R("POST", "/api/auth/login", credentials), 1); assert(login.status == 200);
  for (unsigned i = 0; i < 12; i++) {
    DynamicJsonDocument body(1024); char mac[13]; snprintf(mac, sizeof(mac), "aabbccdd%04x", i + 1);
    body["mac"] = std::string(mac); body["model"] = i == 0 ? "tc1" : i == 1 ? "a1" : "m1"; body["version"] = "v2";
    std::string request; serializeJson(body, request);
    assert(service.handle(R("POST", "/api/devices", request, login.token), i + 2).status == 201);
    assert(config.capacity() < 16384 && iotcloud::HubService::valid(config["hub"]));
    auto expected = snapshot(); boot(nor); assert(loadConfigText() == expected); intact();
  }
  DynamicJsonDocument body(8192); body["enabled"] = true; body["api_key"] = "test-api-key-1234";
  auto maps = body.createNestedArray("mappings");
  for (unsigned i = 0; i < 8; i++) {
    auto map = maps.createNestedObject(); unsigned device = i < 6 ? 0 : 1, channel = i < 6 ? i : i - 6;
    map["device"] = config["hub"]["devices"][device]; map["channel"] = channel;
    map["topic"] = std::string("lab") + std::to_string(i) + (device == 0 ? "001" : channel ? "002" : "003");
  }
  std::string request; serializeJson(body, request);
  auto reply = service.handle(R("PUT", "/api/integrations/bemfa", request, login.token), 20);
  assert(reply.status == 200);
  // Keeping the key must copy it before move-assignment releases the old pool.
  body["api_key"] = ""; request.clear(); serializeJson(body, request);
  assert(service.handle(R("PUT", "/api/integrations/bemfa", request, login.token), 21).status == 200);
  assert(config["hub"]["bemfa"]["api_key"] == "test-api-key-1234");
  body.clear(); body["enabled"] = true; maps = body.createNestedArray("mappings");
  for (unsigned i = 0; i < 8; i++) {
    auto map = maps.createNestedObject(); map["direction"] = "out";
    map["local_topic"] = std::string("lab/local/") + std::to_string(i);
    map["remote_topic"] = std::string("lab/upstream/") + std::to_string(i);
    map["retain"] = false; map["allow_retained"] = false;
  }
  request.clear(); serializeJson(body, request);
  assert(service.handle(R("PUT", "/api/hub/bridge", request, login.token), 22).status == 200);
  auto expected = snapshot(); boot(nor); assert(loadConfigText() == expected); intact();
  printf("12 devices + 8 Bemfa + 8 bridge mappings: JSON=%zu pool=%zu/16384 bytes\n", expected.size(), config.capacity());
}
static void fallback() {
  Nor nor; seed(nor); assert(loadConfigText() == legacy());
  // A historical blob without a colliding STR remains readable.
  Nor blob; boot(blob); std::string text = R"({"_version":1,"indicator":true})";
  assert(prefs.putBytes("config", text.data(), text.size()) == text.size());
  assert(loadConfigText() == text);
  assert(prefs.putBytes("config_hub", "{}", 2) == 2);
#ifdef IOTCLOUD_HUB
  assert(loadConfigText() == "{}");
#else
  assert(loadConfigText() == text);
#endif
}
static void fullSave() {
  Nor nor; seed(nor); load(); assert(config.capacity() < configCapacity);
  DynamicJsonDocument candidate(configCapacity); assert(candidate.set(config));
  candidate["device"][std::string("name")] = std::string(63, 'f');
  candidate["padding"] = std::string();
  auto base = measureJson(candidate); assert(base < 3999);
  candidate["padding"] = std::string(3999 - base, 'p');
  assert(measureJson(candidate) == 3999);
  assert(saveConfig(candidate)); assert(candidate.capacity() == 0);
  const auto committed = snapshot(); assert(committed.size() == 3999);
  assert(prefs.getType("config") == PT_STR && !prefs.isKey("config_hub"));
  assert(config.capacity() < configCapacity); wifiIntact();
  boot(nor); assert(loadConfigText() == committed); load();
  assert(config["device"]["name"] == std::string(63, 'f'));
  DynamicJsonDocument oversized(configCapacity);
  auto larger = committed; auto padding = larger.find(std::string(32, 'p'));
  assert(padding != std::string::npos); larger.insert(padding, 1, 'p');
  assert(!deserializeJson(oversized, larger));
  assert(measureJson(oversized) == 4000); nor.work = 0;
  assert(!saveConfig(oversized)); assert(nor.work == 0);
  assert(snapshot() == committed && loadConfigText() == committed);
  DynamicJsonDocument rejected(configCapacity); assert(rejected.set(config));
  rejected["device"][std::string("name")] = "failed-update";
  const auto updates = passwordUpdates; nor.fail = true;
  assert(!saveConfig(rejected)); nor.fail = false;
  assert(rejected.capacity() == configCapacity && passwordUpdates == updates);
  assert(snapshot() == committed); boot(nor);
  assert(loadConfigText() == committed); wifiIntact();
  printf("full ordinary STR save/reboot: 3999 accepted, 4000 and Flash error preserve old state\n");
}
int main(int argc, char **argv) {
  assert(argc == 2); const std::string scenario = argv[1];
  if (scenario == "collision") collision();
  else if (scenario == "migration") migration();
  else if (scenario == "invalid") invalid();
  else if (scenario == "power") { powerCase(false); powerCase(true); }
  else if (scenario == "budget") budget();
  else if (scenario == "failure") failure();
  else if (scenario == "short") shortWrite();
  else if (scenario == "fallback") fallback();
  else if (scenario == "full") fullSave();
  else assert(false);
  storage.reset();
}
