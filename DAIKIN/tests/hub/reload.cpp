// The production capture, poll and both SDK start/stop paths run unchanged.
// SDK stop injects a final callback before joining its task, as real stop can.
#include "hub_relay.h"
#include "hub_transport.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <deque>
#include <memory>

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
static std::deque<HubMessage> queue;
static unsigned queueResets = 0, stops = 0, starts = 0, destroyed = 0;
static int xQueueSend(QueueHandle_t, const HubMessage *message, unsigned) {
  queue.push_back(*message);
  return pdTRUE;
}
static int xQueueReceive(QueueHandle_t, HubMessage *message, unsigned) {
  if (queue.empty()) return 0;
  *message = queue.front();
  queue.pop_front();
  return pdTRUE;
}
struct Client { bool alive = false, old = false; unsigned source = 0; };
static Client clients[8];
static unsigned allocated = 0;
static void xQueueReset(QueueHandle_t) {
  for (const auto &client : clients) assert(!client.old || !client.alive);
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
    assert(hubMessages[client.source].receiving);
    if (!client.source) {
      mqttQueued = mqttReceiving = mqttSensorCommand = true;
      mqttReceived = 7;
      strcpy(mqttPending, "old-command");
    }
  }
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

int main(int argc, char **argv) {
  assert(argc == 2);
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
  hubMessages.reset(new HubMessage[3]);hubInbox = reinterpret_cast<void *>(1);
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
  for (unsigned i = 0; i < 3; ++i) assert(!hubMessages[i].receiving && !hubMessages[i].length);
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
