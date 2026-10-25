/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// Observe successful real host allocations. Never deny or alter an allocation.
// WString buffers use realloc. JSON pools are marked by a no-data observer
// subclass after the real DynamicJsonDocument constructor has allocated them.
// Neither allocation sizes nor attempt counts identify a JSON pool.
struct Allocation { void *pointer; size_t size; bool string, json; };
static Allocation allocations[2048] = {};
struct PoolAttempt { size_t capacity; unsigned phase; };
static PoolAttempt attempts[32] = {};
static size_t poolLive = 0, poolPeak = 0, attemptCount = 0, newCalls = 0;
static bool inPublish = false, observeNew = false;
static const void *firstWire = nullptr;
static bool firstWireFreed = false;
static unsigned published = 0;
static size_t stringBaseline = 0;
static void *lastMalloc = nullptr;
static size_t stringBytes() {
  size_t total = 0;
  for (const auto &a : allocations) if (a.pointer && a.string) total += a.size;
  return total;
}
static bool contains(const void *pointer) {
  for (const auto &a : allocations) if (a.pointer == pointer) return true;
  return false;
}
static void forget(void *pointer) {
  if (!pointer) return;
  for (auto &a : allocations) if (a.pointer == pointer) {
    if (a.json) poolLive -= a.size;
    if (inPublish && pointer == firstWire) firstWireFreed = true;
    a = {}; return;
  }
}
static void remember(void *pointer, size_t size, bool string) {
  if (!pointer) return;
  for (auto &a : allocations) if (!a.pointer) {
    a = {pointer, size, string, false};
    return;
  }
  std::abort();
}
extern "C" void *__real_malloc(size_t);
extern "C" void *__real_realloc(void *, size_t);
extern "C" void __real_free(void *);
extern "C" void *__wrap_malloc(size_t size) {
  void *pointer = __real_malloc(size); remember(pointer, size, false);
  lastMalloc = pointer; return pointer;
}
extern "C" void *__wrap_realloc(void *old, size_t size) {
  void *pointer = __real_realloc(old, size);
  if (pointer || !size) { forget(old); remember(pointer, size, true); }
  return pointer;
}
extern "C" void __wrap_free(void *pointer) { forget(pointer); __real_free(pointer); }
void *operator new(size_t size) {
  if (observeNew) ++newCalls;
  if (void *pointer = std::malloc(size ? size : 1)) return pointer;
  std::abort();
}
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }

#include <Arduino.h>
#include <ArduinoJson.h>
// ESP32's unsigned int String::length() is size_t on its 32-bit ABI. Make
// that existing ArduinoJson trait explicit on this 64-bit host, retaining
// the unchanged real String and ArduinoJson serialization implementations.
ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE
namespace string_traits_impl {
template <> struct has_length<::String, void> : true_type {};
}
ARDUINOJSON_END_PRIVATE_NAMESPACE
// This observer retains the actual ArduinoJson pool/default allocator. It
// only marks the allocation just made by the unchanged base constructor.
// Production builder text uses this type alias only inside mqtt.inc below.
class ObservedJsonDocument : public DynamicJsonDocument {
public:
  explicit ObservedJsonDocument(size_t requested) : DynamicJsonDocument(requested) {
    assert(capacity() >= requested && lastMalloc);
    assert(poolLive == 0); // A retry releases its predecessor before allocating.
    if (inPublish && published) {
      assert(published == 1 && firstWire && firstWireFreed);
      assert(stringBytes() == stringBaseline);
    }
    assert(attemptCount < sizeof(attempts) / sizeof(attempts[0]));
    attempts[attemptCount++] = {capacity(), inPublish ? published : 0};
    for (auto &a : allocations) if (a.pointer == lastMalloc) {
      assert(!a.json && !a.string && a.size == capacity());
      a.json = true; poolLive += a.size;
      poolPeak = std::max(poolPeak, poolLive); return;
    }
    std::abort();
  }
};
static_assert(sizeof(ObservedJsonDocument) == sizeof(DynamicJsonDocument),
              "The JSON observer must not add instance data");
#include "hub_native_publish.h"
#include "hub_service.h"
#include "frames.h"
#include "cm1106.h"
#include "indicator_json.h"

static uint32_t clockMs = 240000;
static uint32_t millis() { return clockMs; }
enum Sensor { PM25, CO2, TEMPERATURE, HUMIDITY, TVOC, HCHO, PM10, SENSOR_COUNT };
static daikin::Sample readings[SENSOR_COUNT];
static const char *keys[] = {"pm25", "co2", "temperature", "humidity", "tvoc", "formaldehyde", "pm10"};
static const char *units[] = {"µg/m³", "ppm", "°C", "%", "ppb", "mg/m³", "µg/m³"};
static bool hchoPresent = true, indicator = true, configRecovery = false, hubRestoring = false;
static bool customLightSettings = false;
static daikin::LightSettings lightSettings = daikin::defaultLightSettings();
static bool sensorPresent(unsigned i) { return i == HCHO ? hchoPresent : i != PM10 || readings[PM10].received; }
static daikin::CM1106 co2Module;
static const char *pmModel = "PM2105", *version = "1.0.0-native-test";
static constexpr const char *DAIKIN_VARIANT = "hub";
static String deviceId("aabbccddeeff"), apName("DAIKIN-Test"), mqttRoot, sensorRoot;
static uint32_t sensorBoot = 0x80000011, sensorSequence = 0x80000001;
static DynamicJsonDocument config(16384);
static iotcloud::HubService hubService(config);
struct HostTransport {
  iotcloud::HubBroker broker;
  bool running() const { return true; }
} hubTransport;
static constexpr unsigned MALLOC_CAP_INTERNAL = 2048, MALLOC_CAP_8BIT = 4;
static size_t heap_caps_get_free_size(unsigned) { return 200000; }
static size_t heap_caps_get_largest_free_block(unsigned) { return 60000; }
static size_t heap_caps_get_minimum_free_size(unsigned) { return 150000; }
#define hubAllocationReady actualHubAllocationReady
#include "allocation.inc"
#undef hubAllocationReady
static std::vector<size_t> checks;
static bool hubAllocationReady(size_t n) { checks.push_back(n); return actualHubAllocationReady(n); }
static bool hubRestartPending() { return false; }
#include "ready.inc"

// Hardware/SDK boundaries supply ordinary fixed state, not replacement JSON.
struct { bool getBool(const char *, bool value) const { return value; } } prefs;
struct {
  unsigned getFreeHeap() const { return 200000; }
  unsigned getMinFreeHeap() const { return 150000; }
  unsigned getMaxAllocHeap() const { return 60000; }
} ESP;
enum { WL_CONNECTED = 3, WIFI_AP = 2, LOW = 0 };
struct HostIP { String toString() const { return String("192.0.2.10"); } };
struct {
  int status() const { return WL_CONNECTED; }
  HostIP localIP() const { return {}; }
  HostIP softAPIP() const { return {}; }
  int RSSI() const { return -42; }
  unsigned getMode() const { return 1; }
} WiFi;
static int digitalRead(int) { return 1; }
static void digitalWrite(unsigned, bool) {}
static unsigned esp_reset_reason() { return 1; }
struct Partition { const char *label; unsigned type, subtype, address, size; };
static const Partition partitions[] = {
  {"nvs", 1, 2, 0x9000, 0x4000}, {"app0", 0, 16, 0x10000, 0x180000},
  {"app1", 0, 17, 0x190000, 0x180000}};
static constexpr unsigned ESP_PARTITION_TYPE_ANY = 255, ESP_PARTITION_SUBTYPE_ANY = 255;
static const Partition *esp_ota_get_running_partition() { return &partitions[1]; }
static const Partition *esp_ota_get_next_update_partition(void *) { return &partitions[2]; }
static const Partition *esp_partition_find(unsigned, unsigned, void *) { return partitions; }
static const Partition *esp_partition_get(const Partition *p) { return p; }
static const Partition *esp_partition_next(const Partition *p) { return p == &partitions[2] ? nullptr : p + 1; }
static void esp_partition_iterator_release(const Partition *) {}
static int mqttLastEvent = 1, mqttErrorType = 0, mqttSocketError = 0;
static bool mqttConnected = true;
#include "indicators.inc"
#include "state.inc"
#include "typed.inc"
#include "native.inc"

struct Message { std::string topic, body; bool retained; };
static std::vector<Message> localMessages, sdkMessages;
static void *mqtt = reinterpret_cast<void *>(1);
static int esp_mqtt_client_publish(void *, const char *topic, const char *body,
                                   size_t length, int qos, bool retain) {
  assert(poolLive == 0 && qos == 1);
  assert(topic && body && std::strlen(body) == length);
  if (inPublish) {
    if (!published) firstWire = body;
    ++published;
  }
  sdkMessages.push_back({topic, std::string(body, length), retain});
  return 1;
}
#define DynamicJsonDocument ObservedJsonDocument
#include "mqtt.inc"
#undef DynamicJsonDocument

// Ordinary HTTP status requests use the actual handler/response functions and
// the actual loop's status branch. The host delay only pumps that loop work.
using esp_err_t = int;
static constexpr int ESP_OK = 0;
struct httpd_req_t {};
struct { std::string status, type, body; } httpResponse;
static int httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) { return ESP_OK; }
static int httpd_resp_set_status(httpd_req_t *, const char *status) { httpResponse.status = status; return ESP_OK; }
static int httpd_resp_set_type(httpd_req_t *, const char *type) { httpResponse.type = type; return ESP_OK; }
static int httpd_resp_send(httpd_req_t *, const char *body, size_t length) {
  assert(poolLive == 0 && body && std::strlen(body) == length);
  httpResponse.body.assign(body, length); return ESP_OK;
}
#include "json-response.inc"
static bool authorized(httpd_req_t *) { return true; }
static bool webPending = false, webDone = true;
static String webBody, webResult;
static unsigned webOperation = 0;
static int webMux = 0;
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
static void statusWork() {
  assert(webPending && webOperation == 4 && !webDone);
#include "status-work.inc"
  webPending = false; webDone = true;
}
static void delay(unsigned) { statusWork(); }
#include "status-handler.inc"

static std::string prefix(const std::string &name) {
  return "phicomm/v2/" + iotcloud::HubService::userKey(name) + "/";
}
static void account(const std::string &upstream, const std::string &local) {
  config.clear();
  config["hub"]["username"] = local;
  assert(iotcloud::HubService::nameValid(config["hub"]["username"]));
  config["wifi"]["ssid"] = "normal-fixture";
  config["device"]["name"] = "Bedroom 中文";
  config["device"]["language"] = "en";
  config["mqtt"]["user"] = upstream;
  config["mqtt"]["host"] = "example.invalid";
  const auto root = upstream.empty() ? "phicomm/v2/local/" : prefix(upstream);
  mqttRoot = (root + "daikin/aabbccddeeff").c_str();
  sensorRoot = (root + "sensor/aabbccddeeff").c_str();
  hubTransport.broker.reset();
  hubTransport.broker.authorize = [](const std::string &topic, bool write) { return hubService.allowed(topic, write); };
  hubTransport.broker.allocate = [](size_t n) { return hubAllocationReady(n); };
  hubTransport.broker.event = [](const std::string &topic, const std::string &body, bool retain, iotcloud::HubBroker::Origin origin) {
    assert(poolLive == 0 && origin == iotcloud::HubBroker::Origin::Internal);
    localMessages.push_back({topic, body, retain});
  };
  localMessages.clear(); sdkMessages.clear(); checks.clear();
}
static void sensors() {
  const float values[] = {12, 721, 24.5f, 46.25f, 108, .04f, 23};
  for (unsigned i = 0; i < SENSOR_COUNT; ++i) readings[i].set(values[i], clockMs - 100);
  readings[TVOC].at = clockMs - 21000; // Ordinary stale sample, retained as null.
  co2Module.received = co2Module.warmed = true;
  co2Module.raw = 721; co2Module.measuredAt = clockMs - 100;
  pollIndicators(clockMs);
}
static void namespaces() {
  for (const auto &upstream : {std::string(), std::string("cloud-user"), std::string(63, 'U'), std::string(32, 'L')}) {
    account(upstream, std::string(32, 'L'));
    const auto local = prefix(std::string(32, 'L'));
    const std::string body = "{\"sensors\":{\"temperature\":{\"value\":24.5,\"valid\":true}},\"name\":\"客厅\"}";
    hubLocalPublish(mqttRoot + "/state", String(body.c_str()), true);
    assert(localMessages.size() == 1 && localMessages[0].topic == local + "daikin/aabbccddeeff/state");
    assert(localMessages[0].body == body && localMessages[0].retained);
    assert(checks[0] == 2048 && checks[1] == 2 * (localMessages[0].topic.size() + body.size()) + 2048);
    std::string typed = "{\"schema\":1,\"model\":\"daikin\",\"reference\":\"" + std::string(mqttRoot.c_str()) + "/state\"}";
    hubLocalPublish(sensorRoot + "/state", String(typed.c_str()), false);
    assert(localMessages[1].topic == local + "sensor/aabbccddeeff/state" && !localMessages[1].retained);
    assert(localMessages[1].body.find(local + "daikin/aabbccddeeff/state") != std::string::npos);
  }
}
static std::string discoveryBody(const std::string &key) {
  const auto root = "phicomm/v2/" + key + "/";
  return "{\"state_topic\":\"" + root + "daikin/aabbccddeeff/state\",\"availability_topic\":\"" + root +
    "daikin/aabbccddeeff/availability\",\"json_attributes_topic\":\"" + root +
    "daikin/aabbccddeeff/state\",\"unique_id\":\"" + key + "_daikin_aabbccddeeff_pm25\",\"device\":{\"identifiers\":[\"" +
    key + "_daikin_aabbccddeeff\"]},\"name\":\"Bedroom 中文\"}";
}
static void discovery() {
  for (const auto &upstream : {std::string(), std::string("old"), std::string(63, 'U')}) {
    const std::string local(32, 'L'); account(upstream, local);
    const auto up = upstream.empty() ? std::string("local") : iotcloud::HubService::userKey(upstream);
    const auto key = iotcloud::HubService::userKey(local);
    const std::string topic = "homeassistant/sensor/" + up + "/daikin_aabbccddeeff_pm25/config";
    hubLocalPublish(String(topic.c_str()), String(discoveryBody(up).c_str()), true);
    assert(localMessages.size() == 1);
    assert(localMessages[0].topic == "homeassistant/sensor/" + key + "/daikin_aabbccddeeff_pm25/config");
    assert(localMessages[0].body == discoveryBody(key));
  }
}
static void retained() {
  account("old", "owner");
  const auto up = iotcloud::HubService::userKey("old");
  const std::string topic = "homeassistant/sensor/" + up + "/daikin_aabbccddeeff_pm25/config";
  publish(String(topic.c_str()), String(discoveryBody(up).c_str()), true);
  assert(hubTransport.broker.retainedBytes() > 0);
  const String empty(""); assert(empty && empty.length() == 0);
  publish(String(topic.c_str()), empty, true);
  assert(localMessages.size() == 2 && localMessages.back().body.empty() && localMessages.back().retained);
  assert(sdkMessages.size() == 2 && sdkMessages.back().body.empty() && sdkMessages.back().retained);
  assert(hubTransport.broker.retainedBytes() == 0);
}
static void render() {
  const std::string from = "phicomm/v2/local/", to = prefix(std::string(32, 'L'));
  const iotcloud::NativeRule rules[] = {{from, to}};
  const std::string input = from + "m1/aabbccddeeff/state " + from + "sensor/aabbccddeeff/state 中文";
  const std::string expected = to + "m1/aabbccddeeff/state " + to + "sensor/aabbccddeeff/state 中文";
  size_t length = 0;
  assert(iotcloud::nativeMessageSize(input.data(), input.size(), rules, 1, 8192, length));
  assert(length == expected.size());
  std::string out; out.reserve(length); newCalls = 0; observeNew = true;
  iotcloud::nativeMessageAppend(out, input.data(), input.size(), rules, 1);
  observeNew = false; assert(out == expected && newCalls == 0);
  size_t emptyLength = 42;
  assert(iotcloud::nativeMessageSize("", 0, rules, 1, 8192, emptyLength) && emptyLength == 0);
}
static void verifySnapshots() {
  assert(localMessages.size() == 2 && sdkMessages.size() == 2);
  assert(localMessages[0].topic == prefix("owner") + "daikin/aabbccddeeff/state");
  assert(localMessages[1].topic == prefix("owner") + "sensor/aabbccddeeff/state");
  assert(sdkMessages[0].topic == std::string(mqttRoot.c_str()) + "/state");
  assert(sdkMessages[1].topic == std::string(sensorRoot.c_str()) + "/state");
  assert(localMessages[0].retained && !localMessages[1].retained);
  assert(sdkMessages[0].retained && !sdkMessages[1].retained);
  assert(localMessages[0].body == sdkMessages[0].body && localMessages[1].body == sdkMessages[1].body);
  DynamicJsonDocument full(8192), typed(8192);
  assert(!deserializeJson(full, localMessages[0].body));
  assert(!deserializeJson(typed, localMessages[1].body));
  assert(full["model"] == "daikin" && full["firmware_variant"] == "hub");
  assert(full["device_id"] == "aabbccddeeff" && full["name"] == "Bedroom 中文");
  assert(full["co2_diagnostics"]["raw_ppm"] == 721);
  assert(full["ota_partitions"]["entries"].size() == 3);
  assert(full["indicator_rules_supported"] == true);
  assert(full["sensors"].size() == SENSOR_COUNT && typed["sensors"].size() == SENSOR_COUNT);
  assert(typed["schema"] == 1 && typed["model"] == "daikin" && typed["device_id"] == "aabbccddeeff");
  assert(typed["boot"].as<uint32_t>() == sensorBoot && typed["boot"].as<uint64_t>() == sensorBoot);
  assert(typed["sequence"].as<uint32_t>() == sensorSequence && typed["sequence"].as<uint64_t>() == sensorSequence);
  for (unsigned i = 0; i < SENSOR_COUNT; ++i) {
    const auto a = full["sensors"][keys[i]], b = typed["sensors"][keys[i]];
    assert(a["unit"] == units[i] && b["unit"] == units[i]);
    assert(a["valid"].as<bool>() == (i != TVOC) && b["valid"].as<bool>() == (i != TVOC));
    if (i == TVOC) assert(a["value"].isNull() && b["value"].isNull());
    else assert(a["value"].as<float>() == readings[i].value && b["value"].as<float>() == readings[i].value);
    assert(a["age_ms"].as<uint32_t>() == b["age_ms"].as<uint32_t>());
  }
}
struct DocumentStats { size_t memory, wire, slots; };
static size_t documentSlots(JsonVariantConst value) {
  size_t count = 0;
  if (value.is<JsonObjectConst>()) {
    for (JsonPairConst item : value.as<JsonObjectConst>())
      count += 1 + documentSlots(item.value());
  } else if (value.is<JsonArrayConst>()) {
    for (JsonVariantConst item : value.as<JsonArrayConst>())
      count += 1 + documentSlots(item);
  }
  return count;
}
static DocumentStats measureDocument(void (*fill)(JsonObject)) {
  DynamicJsonDocument doc(8192);
  fill(doc.to<JsonObject>());
  assert(!doc.overflowed());
  return {doc.memoryUsage(), measureJson(doc), documentSlots(doc.as<JsonVariantConst>())};
}
static void resetPoolObservation() {
  assert(!inPublish && poolLive == 0);
  poolPeak = attemptCount = 0; published = 0;
  firstWire = nullptr; firstWireFreed = false;
}
static size_t verifyAttempts(unsigned phase, size_t initial, size_t required) {
  size_t previous = 0, count = 0;
  for (size_t i = 0; i < attemptCount; ++i) if (attempts[i].phase == phase) {
    assert(attempts[i].capacity == (previous ? previous * 2 : initial));
    assert(attempts[i].capacity <= 8192);
    previous = attempts[i].capacity; ++count;
  }
  size_t enough = initial;
  while (enough < required && enough < 8192) enough *= 2;
  assert(count && previous == enough && previous >= required);
  return previous;
}
static void printAttempts(const char *kind) {
  std::printf("%s JSON attempts:", kind);
  for (size_t i = 0; i < attemptCount; ++i)
    std::printf(" %u:%zu", attempts[i].phase, attempts[i].capacity);
  std::printf("; max live pool=%zu, pool bytes at publisher/HTTP send=0\n", poolPeak);
}
static void httpStatusRead(const DocumentStats &required) {
  String expected("previous ordinary response");
  assert(mqttStateJson(expected, state));
  assert(poolLive == 0);
  const std::string expectedData(expected.c_str());
  resetPoolObservation();
  webResult = "{\"code\":\"ok\",\"data\":{\"previous\":true}}";
  httpResponse = {}; httpd_req_t request;
  assert(statusHandler(&request) == ESP_OK);
  assert(!webPending && webDone && webOperation == 4 && poolLive == 0);
  assert(httpResponse.status == "200 OK" && httpResponse.type == "application/json");
  const auto finalCapacity = verifyAttempts(0, 4096, required.memory);
  assert(poolPeak == finalCapacity);
  DynamicJsonDocument response(8192);
  assert(!deserializeJson(response, httpResponse.body));
  assert(response.size() == 2 && response["code"] == "ok");
  assert(response["data"].is<JsonObject>() && response["data"]["sensors"].size() == SENSOR_COUNT);
  String data; serializeJson(response["data"], data);
  assert(std::string(data.c_str()) == expectedData);
  assert(httpResponse.body.size() == required.wire);
  printAttempts("normal HTTP status");
}
static DocumentStats measuredFull, measuredTyped;
static void documentMeasurements() {
  const auto state0 = measureDocument(state), typed0 = measureDocument(sensorBody), http0 = measureDocument(httpStatusBody);
  httpStatusRead(http0);
  auto devices = config["hub"].createNestedArray("devices");
  const char *models[] = {"m1", "a1", "tc1"};
  const char *macs[] = {"112233445566", "223344556677", "334455667788"};
  for (unsigned i = 0; i < 3; ++i) {
    auto device = devices.createNestedObject();
    device["model"] = models[i]; device["mac"] = macs[i];
    device["version"] = "v2"; device["status"] = "active";
    device["username"] = "owner";
    assert(iotcloud::HubService::deviceValid(device));
  }
  const auto state3 = measureDocument(state), typed3 = measureDocument(sensorBody), http3 = measureDocument(httpStatusBody);
  assert(state0.memory == state3.memory && state0.wire == state3.wire && state0.slots == state3.slots);
  assert(typed0.memory == typed3.memory && typed0.wire == typed3.wire && typed0.slots == typed3.slots);
  assert(http0.memory == http3.memory && http0.wire == http3.wire && http0.slots == http3.slots);
  httpStatusRead(http3);
  measuredFull = state3; measuredTyped = typed3;
  const size_t slotBytes = sizeof(ArduinoJson::detail::VariantSlot);
  for (unsigned registered : {0u, 3u}) {
    const auto &full = registered ? state3 : state0;
    const auto &typed = registered ? typed3 : typed0;
    const auto &http = registered ? http3 : http0;
    std::printf("normal document metrics registered=%u host_slot=%zu full memoryUsage=%zu measureJson=%zu slots=%zu copied_string_bytes=%zu; typed memoryUsage=%zu measureJson=%zu slots=%zu copied_string_bytes=%zu; HTTP envelope memoryUsage=%zu measureJson=%zu slots=%zu copied_string_bytes=%zu\n",
                registered, slotBytes, full.memory, full.wire, full.slots, full.memory - full.slots * slotBytes,
                typed.memory, typed.wire, typed.slots, typed.memory - typed.slots * slotBytes,
                http.memory, http.wire, http.slots, http.memory - http.slots * slotBytes);
  }
  config["hub"].remove("devices");
}
static void statePublication(unsigned repetitions) {
  account("cloud-user", "owner"); sensors();
  documentMeasurements();
  for (unsigned i = 0; i < repetitions; ++i) {
    localMessages.clear(); sdkMessages.clear(); checks.clear();
    resetPoolObservation();
    const uint32_t previousSequence = sensorSequence;
    stringBaseline = stringBytes(); inPublish = true;
    publishState();
    inPublish = false;
    assert(poolLive == 0 && published == 2);
    const auto fullCapacity = verifyAttempts(0, 4096, measuredFull.memory);
    const auto typedCapacity = verifyAttempts(1, 1024, measuredTyped.memory);
    assert(poolPeak == std::max(fullCapacity, typedCapacity));
    assert(stringBytes() == stringBaseline && firstWire && firstWireFreed);
    assert(sensorSequence && sensorSequence > previousSequence);
    verifySnapshots();
  }
  std::printf("normal publications=%u full wire=%zu typed wire=%zu; first wire released before all typed pool attempts\n",
              repetitions, sdkMessages[0].body.size(), sdkMessages[1].body.size());
  printAttempts("normal state/typed");
}
static void ready() {
  account(std::string(63, 'U'), std::string(32, 'L'));
  newCalls = 0; observeNew = true;
  for (unsigned i = 0; i < 100; ++i) assert(hubService.ready() && hubReady());
  observeNew = false; assert(newCalls == 0);
  config.remove("hub"); // Ordinary owner setup has not completed yet.
  newCalls = 0; observeNew = true;
  for (unsigned i = 0; i < 100; ++i) assert(!hubService.ready() && !hubReady());
  observeNew = false; assert(newCalls == 0);
}
int main(int argc, char **argv) {
  assert(argc == 2); const std::string name = argv[1];
  if (name == "namespaces") namespaces();
  else if (name == "discovery") discovery();
  else if (name == "retained") retained();
  else if (name == "render") render();
  else if (name == "state") statePublication(1);
  else if (name == "repeat") statePublication(40);
  else if (name == "ready") ready();
  else std::abort();
}
