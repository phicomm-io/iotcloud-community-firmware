// The production capture, poll and both SDK start/stop paths run unchanged.
// SDK stop injects a final callback before joining its task, as real stop can.
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <new>
// Watch actual dynamic requests for HubMessage. The by-value queue below is
// fixed storage, so source/dequeue allocations cannot be confused with nodes.
static size_t messageSize = 0;
static std::atomic<size_t> liveMessageBytes{0}, peakMessageBytes{0}, messageAttempts{0}, deniedMessages{0};
static std::atomic<bool> failNextMessage{false};
struct MessageAllocation { void *pointer; size_t bytes; };
static MessageAllocation messageAllocations[8] = {};
static std::mutex allocationMutex;
void *operator new(size_t n) {
  if (messageSize && n == messageSize) {
    ++messageAttempts;
    if (failNextMessage.exchange(false)) { ++deniedMessages; throw std::bad_alloc(); }
  }
  void *p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  if (messageSize && n == messageSize) {
    std::lock_guard<std::mutex> lock(allocationMutex);
    bool saved = false;
    for (auto &a : messageAllocations) if (!a.pointer) {
      a = {p, n}; saved = true; break;
    }
    if (!saved) std::abort();
    const size_t live = liveMessageBytes.fetch_add(n) + n;
    size_t peak = peakMessageBytes;
    while (peak < live && !peakMessageBytes.compare_exchange_weak(peak, live)) {}
  }
  return p;
}
void operator delete(void *p) noexcept {
  if (!p) return;
  {
    std::lock_guard<std::mutex> lock(allocationMutex);
    for (auto &a : messageAllocations) if (a.pointer == p) {
      liveMessageBytes -= a.bytes; a = {}; break;
    }
  }
  std::free(p);
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void *operator new(size_t n, const std::nothrow_t &) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void operator delete(void *p, const std::nothrow_t &) noexcept { ::operator delete(p); }
#include "hub_relay.h"
#include "hub_transport.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>

#define IOTCLOUD_HUB 1
struct String : std::string {
  using std::string::string;
  String(const std::string &s) : std::string(s) {}
  size_t length() const { return size(); }
};
using esp_mqtt_client_handle_t = void *;
using QueueHandle_t = void *;
using esp_event_base_t = const char *;
struct Event {
  int current_data_offset = 0;
  char *topic = nullptr;
  int topic_len = 0, total_data_len = 0, data_len = 0;
  char *data = nullptr;
  bool retain = false;
};
using esp_mqtt_event_handle_t = Event *;
struct esp_mqtt_client_config_t {
  const char *host = nullptr, *username = nullptr, *password = nullptr,
             *client_id = nullptr, *lwt_topic = nullptr, *lwt_msg = nullptr;
  int port = 0, lwt_qos = 0, lwt_retain = 0, buffer_size = 0,
      out_buffer_size = 0, keepalive = 0;
};
static constexpr int MQTT_EVENT_ANY = -1, pdTRUE = 1;
static DynamicJsonDocument config(16384);
static iotcloud::HubService hubService(config);
static iotcloud::HubTransport hubTransport;
static iotcloud::HubRelay hubRelay(config, hubService, hubTransport.broker);
static bool configRecovery = false, mqttRestart = false, otaActive = false,
            mqttConnected = false, mqttCommandReady = false, mqttOnline = false,
            haBirth = false, mqttReceiving = false, mqttQueued = false,
            mqttSensorCommand = false;
static uint32_t nextPublish = 0;
static size_t mqttReceived = 0;
static char mqttPending[4097] = {};
static int mqttMux = 0;
#define portENTER_CRITICAL(mux) (void)(mux)
#define portEXIT_CRITICAL(mux) (void)(mux)
struct SensorSession { bool used = false; };
static SensorSession sessions[4];
static esp_mqtt_client_handle_t mqtt = nullptr;
static String deviceId = "aabbccddeeff", mqttRoot, sensorRoot;
#include "globals.inc"
static size_t availableHeap = size_t(-1), largestBlock = size_t(-1);
static size_t hubByteHeap() { return availableHeap > liveMessageBytes ? availableHeap - liveMessageBytes : 0; }
static size_t hubByteBlock() { return largestBlock; }
#include "allocation.inc"
struct ValueQueue {
  HubMessage values[3];
  size_t head = 0, count = 0;
  bool empty() const { return !count; }
  size_t size() const { return count; }
  const HubMessage &front() const { assert(count); return values[head]; }
  const HubMessage &back() const { assert(count); return values[(head + count - 1) % 3]; }
  bool push_back(const HubMessage &message) {
    if (count == 3) return false;
    values[(head + count++) % 3] = message; return true;
  }
  void pop_front() { assert(count); head = (head + 1) % 3; --count; }
  void clear() { head = count = 0; }
};
static ValueQueue queue;
static std::mutex queueMutex;
static unsigned queueResets = 0, receives = 0, stops = 0, starts = 0, destroyed = 0;
static int xQueueSend(QueueHandle_t, const HubMessage *message, unsigned) {
  std::lock_guard<std::mutex> lock(queueMutex);
  return queue.push_back(*message) ? pdTRUE : 0;
}
static int xQueueReceive(QueueHandle_t, HubMessage *message, unsigned) {
  std::lock_guard<std::mutex> lock(queueMutex);
  if (queue.empty()) return 0;
  ++receives;
  *message = queue.front();
  queue.pop_front();
  return pdTRUE;
}
static size_t uxQueueMessagesWaiting(QueueHandle_t) {
  std::lock_guard<std::mutex> lock(queueMutex); return queue.size();
}
struct Client { bool alive = false, old = false; unsigned source = 0; std::thread *task = nullptr; };
static Client clients[8];
static unsigned allocated = 0;
static void xQueueReset(QueueHandle_t) {
  for (const auto &client : clients) assert(!client.alive && !client.task);
  std::lock_guard<std::mutex> lock(queueMutex);
  ++queueResets;
  queue.clear();
}
#include "capture.inc"
static const std::string command = R"({"request_id":"from-old-client","action":"outlet.set","params":{"index":0,"on":true}})";
static void capture(unsigned source, const std::string &body, bool partial = false) {
  std::string topic = source ? "socket001" : "shared/control";
  Event event;
  event.topic = &topic[0];event.topic_len = topic.size();
  event.data = const_cast<char *>(body.data());
  event.total_data_len = body.size();event.data_len = partial ? 1 : body.size();
  assert(hubCapture(&event, source));
}
static void esp_mqtt_client_stop(esp_mqtt_client_handle_t handle) {
  auto &client = *static_cast<Client *>(handle);
  assert(client.alive);
  ++stops;
  if (client.old) {
    // The old connection is still valid while stop joins its task. These must
    // remain in the old generation and be discarded before new routes run.
    assert(hubEpoch == 0);
    capture(client.source, client.source ? "off" : command);
    assert(queue.back().epoch == 0);
    capture(client.source, client.source ? "off" : command, true);
    assert(hubMessages[client.source] && hubMessages[client.source]->receiving);
    if (!client.source) {
      mqttQueued = mqttReceiving = mqttSensorCommand = true;
      mqttReceived = 7;
      strcpy(mqttPending, "old-command");
    }
  }
  if (client.task) { client.task->join(); client.task = nullptr; }
  client.alive = false;
}
static void esp_mqtt_client_destroy(esp_mqtt_client_handle_t handle) {
  assert(!static_cast<Client *>(handle)->alive);
  ++destroyed;
}
static esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *c) {
  assert(allocated < 8);
  auto &client = clients[allocated++];
  client.source = !strcmp(c->host, "bemfa.com") ? 1 : 0;
  if (!client.source) assert(!strcmp(c->host, "new-broker.example"));
  return &client;
}
using Callback = void (*)(void *, esp_event_base_t, int32_t, void *);
static void esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int, Callback, void *) {}
static void esp_mqtt_client_start(esp_mqtt_client_handle_t handle) {
  for (const auto &client : clients) assert(!client.old || !client.alive);
  assert(hubEpoch > 0 && queueResets == unsigned(hubEpoch));
  auto &client = *static_cast<Client *>(handle);
  client.alive = true;
  if (client.source) bemfaConnected = true;
  else mqttConnected = true;
  ++starts;
}
static void mqttEvent(void *, esp_event_base_t, int32_t, void *) {}
static void bemfaEvent(void *, esp_event_base_t, int32_t, void *) {}
static bool hubRestartPending() { return hubRestoring; }
static bool hubReady() { return hubTransport.running(); }
#include "mqtt.inc"
#include "bemfa.inc"
#include "inbox.inc"
static void homeChanged() {}
#include "poll.inc"

static std::vector<std::string> observed;
static bool fragment(unsigned source, const std::string &topic,
                     const std::string &body, size_t offset, size_t length,
                     bool retain = false) {
  assert(offset <= body.size() && length <= body.size() - offset);
  Event event;
  event.topic = const_cast<char *>(topic.data()); event.topic_len = topic.size();
  event.data = const_cast<char *>(body.data() + offset); event.data_len = length;
  event.total_data_len = body.size(); event.current_data_offset = offset;
  event.retain = retain;
  return hubCapture(&event, source);
}
static void setupInbox() {
  hubInbox = reinterpret_cast<void *>(1);
  hubRemoteTopics = {"shared/control", std::string(256, 't')};
  bemfaTopics = {"socket001"};
  config["hub"]["bridge"]["enabled"] = true;
  hubTransport.broker.authorize = [](const std::string &, bool) { return true; };
  auto mappings = config["hub"]["bridge"]["mappings"].to<JsonArray>();
  for (const auto &topic : hubRemoteTopics) {
    auto route = mappings.createNestedObject();
    route["direction"] = "in"; route["remote_topic"] = topic;
    route["local_topic"] = "local/inbox"; route["allow_retained"] = true;
  }
  hubTransport.broker.event = [](const std::string &topic, const std::string &body,
                                bool, iotcloud::HubBroker::Origin origin) {
    assert(topic == "local/inbox" && origin == iotcloud::HubBroker::Origin::Upstream);
    assert(liveMessageBytes == sizeof(HubMessage)); observed.push_back(body);
  };
  assert(!liveMessageBytes && queue.empty());
  const size_t attempts = messageAttempts;
  for (unsigned i = 0; i < 100; ++i) pollHub(i);
  assert(messageAttempts == attempts && !liveMessageBytes);
}
static void inboxFragments() {
  setupInbox();
  const auto topic = hubRemoteTopics.back();
  std::string binary(4096, 'x'); binary[0] = binary[2048] = '\0'; binary[4095] = char(0xff);
  const std::string expected = binary, off = "off";
  assert(fragment(0, topic, binary, 0, 17, true));
  assert(fragment(1, "socket001", off, 0, 1));
  assert(liveMessageBytes == 2 * messageSize && hubMessages[0] && hubMessages[1]);
  assert(fragment(1, "socket001", off, 1, 2));
  assert(!hubMessages[1] && queue.size() == 1 && liveMessageBytes == messageSize);
  assert(fragment(0, topic, binary, 17, binary.size() - 17, true));
  assert(!hubMessages[0] && !liveMessageBytes && queue.size() == 2);
  assert(queue.front().source == 1 && queue.front().epoch == hubEpoch);
  assert(std::string(queue.front().body, queue.front().length) == off);
  assert(queue.back().source == 0 && queue.back().length == 4096 && queue.back().retain);
  assert(std::string(queue.back().topic) == topic && queue.back().body[4096] == 0);
  std::fill(binary.begin(), binary.end(), 'z');
  assert(std::string(queue.back().body, queue.back().length) == expected);
  pollHub(101); assert(queue.size() == 1 && !liveMessageBytes && observed.empty());
  pollHub(102); assert(queue.empty() && !liveMessageBytes && observed.size() == 1);
  assert(observed[0] == expected && peakMessageBytes == 2 * messageSize);
}
static void inboxInvalid() {
  setupInbox();
  const std::string body = "abcd", known = "shared/control";
  const size_t attempts = messageAttempts;
  assert(!hubCapture(nullptr, 0));
  assert(!fragment(2, known, body, 0, 4));
  assert(!fragment(0, "not-subscribed", body, 0, 4));
  Event event; event.topic = const_cast<char *>(known.data()); event.topic_len = known.size();
  event.data = const_cast<char *>(body.data()); event.data_len = 4; event.total_data_len = 4;
  event.topic_len = -1; assert(!hubCapture(&event, 0));
  event.topic_len = known.size(); event.topic = nullptr; assert(!hubCapture(&event, 0));
  event.topic = const_cast<char *>(known.data());
  event.total_data_len = -1; assert(hubCapture(&event, 0));
  event.total_data_len = 4097; assert(hubCapture(&event, 0));
  event.total_data_len = 4; event.data_len = -1; assert(hubCapture(&event, 0));
  event.data_len = 5; assert(hubCapture(&event, 0));
  event.data_len = 1; event.data = nullptr; assert(hubCapture(&event, 0));
  const std::string longTopic(257, 't'); hubRemoteTopics.push_back(longTopic);
  event = Event(); assert(!hubCapture(&event, 0));
  assert(fragment(0, longTopic, body, 0, 4));
  assert(messageAttempts == attempts && !liveMessageBytes && queue.empty());
  for (unsigned invalid = 0; invalid < 4; ++invalid) {
    assert(fragment(0, known, body, 0, 1));
    event = Event(); event.current_data_offset = 1; event.total_data_len = 4;
    event.data = const_cast<char *>(body.data() + 1); event.data_len = 1;
    if (invalid == 0) event.current_data_offset = 2;
    if (invalid == 1) event.data_len = -1;
    if (invalid == 2) event.data_len = 4;
    if (invalid == 3) event.data = nullptr;
    assert(hubCapture(&event, 0) && !hubMessages[0] && !liveMessageBytes);
    assert(!hubCapture(&event, 0));
  }
  assert(fragment(0, known, body, 0, 1));
  assert(!fragment(0, "not-subscribed", body, 0, 1) && !liveMessageBytes);
  assert(fragment(0, known, body, 0, 1));
  assert(fragment(0, known, body, 0, 2));
  assert(liveMessageBytes == messageSize && hubMessages[0]->length == 2);
  assert(fragment(0, known, body, 2, 2) && !liveMessageBytes);
  queue.clear();
  event = Event(); event.topic = const_cast<char *>(known.data()); event.topic_len = known.size();
  assert(hubCapture(&event, 0) && queue.size() == 1 && !liveMessageBytes);
  assert(queue.front().length == 0 && queue.front().body[0] == 0);
  pollHub(101); assert(queue.empty() && observed.size() == 1 && observed[0].empty());
}
static void inboxAllocation() {
  setupInbox(); const std::string body = "retry";
  availableHeap = messageSize + 16384;
  assert(fragment(0, "shared/control", body, 0, body.size()));
  assert(!messageAttempts && !liveMessageBytes && queue.empty() && hubInboxDropped == 1);
  availableHeap = size_t(-1); largestBlock = messageSize;
  assert(fragment(1, "socket001", body, 0, body.size()));
  assert(!messageAttempts && !liveMessageBytes && queue.empty() && hubInboxDropped == 2);
  largestBlock = size_t(-1); failNextMessage = true;
  assert(fragment(0, "shared/control", body, 0, body.size()));
  assert(deniedMessages == 1 && !liveMessageBytes && queue.empty() && hubInboxDropped == 3);
  assert(fragment(0, "shared/control", body, 0, body.size()));
  assert(queue.size() == 1 && !liveMessageBytes);
  const size_t attempts = messageAttempts;
  availableHeap = messageSize + 16384;
  pollHub(101); assert(queue.size() == 1 && !receives && messageAttempts == attempts);
  availableHeap = size_t(-1); largestBlock = messageSize;
  pollHub(102); assert(queue.size() == 1 && !receives && messageAttempts == attempts);
  largestBlock = size_t(-1); failNextMessage = true;
  pollHub(103); assert(queue.size() == 1 && !receives && deniedMessages == 2);
  assert(std::string(queue.front().body, queue.front().length) == body);
  pollHub(104); assert(queue.empty() && receives == 1 && observed.size() == 1 && observed[0] == body);
  assert(!liveMessageBytes && hubInboxDropped == 3);
}
static void inboxBounds() {
  setupInbox(); const std::string body = "queued";
  for (unsigned i = 0; i < 3; ++i) assert(fragment(0, "shared/control", body, 0, body.size()));
  assert(queue.size() == 3 && !liveMessageBytes);
  assert(fragment(1, "socket001", "off", 0, 3));
  assert(queue.size() == 3 && hubInboxDropped == 1 && !liveMessageBytes);
  for (unsigned i = 0; i < 3; ++i) pollHub(101 + i);
  assert(queue.empty() && receives == 3 && observed.size() == 3);
  assert(fragment(0, "shared/control", body, 0, body.size()));
  ++hubEpoch; pollHub(105);
  assert(queue.empty() && observed.size() == 3 && !liveMessageBytes);
  otaActive = true;
  assert(fragment(0, "shared/control", body, 0, body.size()));
  pollHub(106); assert(queue.empty() && observed.size() == 3 && !liveMessageBytes);
  otaActive = false;
  assert(fragment(0, "shared/control", body, 0, body.size()));
  assert(fragment(0, "shared/control", body, 0, 1));
  assert(fragment(1, "socket001", body, 0, 1));
  hubRestoring = true;
  assert(!fragment(0, "shared/control", body, 1, body.size() - 1));
  pollHub(107); assert(queue.size() == 1 && liveMessageBytes == 2 * messageSize);
  hubResetInbox(); assert(queue.empty() && !liveMessageBytes && hubEpoch == 2);
  for (const auto &message : hubMessages) assert(!message);
}
static void inboxPeak(bool concurrent) {
  setupInbox(); const std::string body = "partial message";
  assert(fragment(0, "shared/control", "queued", 0, 6));
  hubTransport.broker.event = [](const std::string &, const std::string &payload,
                                bool, iotcloud::HubBroker::Origin) {
    assert(payload == "queued" && liveMessageBytes == 3 * messageSize);
    observed.push_back(payload);
  };
  if (concurrent) {
    std::atomic<unsigned> ready{0}; std::atomic<bool> finish{false};
    auto task = [&](unsigned source) {
      const std::string topic = source ? "socket001" : "shared/control";
      assert(fragment(source, topic, body, 0, 1)); ++ready;
      while (!finish) std::this_thread::yield();
      assert(fragment(source, topic, body, 1, body.size() - 1));
    };
    std::thread upstream(task, 0), bemfa(task, 1);
    clients[0].alive = clients[1].alive = true; clients[1].source = 1;
    clients[0].task = &upstream; clients[1].task = &bemfa;
    mqtt = &clients[0]; bemfaMqtt = &clients[1];
    while (ready != 2) std::this_thread::yield();
    assert(liveMessageBytes == 2 * messageSize);
    pollHub(101); assert(queue.empty() && liveMessageBytes == 2 * messageSize);
    finish = true;
    stopMqtt(); stopBemfa();
    assert(queue.size() == 2 && !liveMessageBytes && stops == 2 && destroyed == 2);
  } else {
    assert(fragment(0, "shared/control", body, 0, 1));
    assert(fragment(1, "socket001", body, 0, 1));
    pollHub(101); assert(queue.empty() && liveMessageBytes == 2 * messageSize);
  }
  assert(observed.size() == 1 && observed[0] == "queued");
  assert(peakMessageBytes == 3 * messageSize);
  hubResetInbox(); assert(queue.empty() && !liveMessageBytes);
}
static void inboxScenario(const std::string &name) {
  if (name == "inbox-fragments") inboxFragments();
  else if (name == "inbox-invalid") inboxInvalid();
  else if (name == "inbox-allocation") inboxAllocation();
  else if (name == "inbox-bounds") inboxBounds();
  else { assert(name == "inbox-peak" || name == "inbox-concurrent"); inboxPeak(name == "inbox-concurrent"); }
  printf("PASS %s sizeof=%zu queue_depth=3 idle_live=%zu peak_live=%zu attempts=%zu denied=%zu\n",
         name.c_str(), messageSize, liveMessageBytes.load(), peakMessageBytes.load(),
         messageAttempts.load(), deniedMessages.load());
}
int main(int argc, char **argv) {
  assert(argc == 2);
  messageSize = sizeof(HubMessage);
  if (!strncmp(argv[1], "inbox-", 6)) { inboxScenario(argv[1]); return 0; }
  const bool existing = strcmp(argv[1], "startup");
  const bool native = !strcmp(argv[1], "native");
  assert(!deserializeJson(config,
    R"({"mqtt":{"host":"new-broker.example"},"hub":{"username":"local","devices":[{"mac":"aabbccddeeff","model":"tc1","version":"v2","status":"active","username":"local"}],"bridge":{"enabled":true,"mappings":[]},"bemfa":{"enabled":true,"api_key":"0123456789abcdef","mappings":[{"device":{"mac":"aabbccddeeff","model":"tc1","version":"v2","status":"active","username":"local"},"channel":2,"topic":"socket001"}]}}})"));
  const std::string base = hubService.prefix() + "tc1/aabbccddeeff", target = base + "/command";
  auto route = config["hub"]["bridge"]["mappings"].as<JsonArray>().createNestedObject();
  route["direction"] = "in";route["remote_topic"] = "shared/control";
  route["local_topic"] = target;route["retain"] = false;route["allow_retained"] = false;
  assert(iotcloud::HubService::bridgeValid(config["hub"]["bridge"], "local"));
  assert(hubTransport.start(0, 0));
  unsigned upstreamCommands = 0, bemfaCommands = 0;
  hubTransport.broker.authorize = [](const std::string &t, bool w) { return hubService.allowed(t, w); };
  hubTransport.broker.event = [&](const std::string &t, const std::string &b, bool retain, iotcloud::HubBroker::Origin origin) {
    hubRelay.local(t, b, retain, origin);
    if (t == target && origin == iotcloud::HubBroker::Origin::Upstream) ++upstreamCommands;
    if (t == target && origin == iotcloud::HubBroker::Origin::Bemfa) ++bemfaCommands;
  };
  hubRelay.randomHex = [](size_t n) { return std::string(n * 2, 'a'); };
  hubInbox = reinterpret_cast<void *>(1);
  hubRemoteTopics = {"shared/control"};bemfaTopics = {"socket001"};
  if (existing) {
    allocated = 2;
    clients[0].alive = clients[0].old = clients[1].alive = clients[1].old = true;
    clients[1].source = 1;mqtt = &clients[0];bemfaMqtt = &clients[1];
    capture(0, command);capture(1, "off");
  }
  if (native) {
    hubUpstreamChanged();
    assert(!mqtt && !mqttRestart && hubReload && hubEpoch == 0);
  } else hubReload = true;
  pollHub(100);
  assert(mqttRestart && !mqtt && bemfaMqtt && starts == 1);
  assert(stops == (existing ? 2u : 0u) && destroyed == stops);
  assert(hubEpoch == 1 && queue.empty() && upstreamCommands == 0 && bemfaCommands == 0);
  assert(!mqttQueued && !mqttReceiving && !mqttSensorCommand && !mqttReceived && !mqttPending[0]);
  for (const auto &message : hubMessages) assert(!message);
  // The main loop restarts native MQTT after pollHub. Both new clients still
  // deliver into the new routes; a reload must not turn into a permanent pause.
  startMqtt();
  assert(mqtt && mqttConnected && bemfaConnected && starts == 2);
  mqttRestart = false;
  capture(0, R"({"request_id":"new-client","action":"outlet.set","params":{"index":0,"on":true}})");
  assert(queue.back().epoch == 1);pollHub(101);assert(upstreamCommands == 1);
  hubTransport.broker.publish(base + "/availability", "online", true, iotcloud::HubBroker::Origin::Local);
  hubTransport.broker.publish(base + "/state", R"({"socket_states":[0,0,1,0,0,0]})", true, iotcloud::HubBroker::Origin::Local);
  capture(1, "off");assert(queue.back().epoch == 1);pollHub(102);assert(bemfaCommands == 1);
  // A second reload must also complete with clients created by the real start
  // functions, rather than only with the pre-existing connection fixture.
  if (native) {
    capture(0, command);capture(0, command);
    capture(1, "off", true);
    mqttQueued = mqttReceiving = true;mqttReceived = 4;strcpy(mqttPending, "old");
    hubUpstreamChanged();
    assert(!mqtt && !mqttRestart && hubReload && hubEpoch == 1);
    assert(!mqttQueued && !mqttReceiving && !mqttReceived);
  } else hubReload = true;
  pollHub(103);startMqtt();
  assert(hubEpoch == 2 && queueResets == 2 && starts == 4 && mqtt && bemfaMqtt);
  assert(upstreamCommands == 1 && bemfaCommands == 1);
  capture(0, "fresh after second reload");pollHub(104);assert(upstreamCommands == 2);
  stopMqtt();stopBemfa();
  puts(existing ? "production hub reload generation passed" : "production hub startup/reload passed");
}
