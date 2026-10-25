/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <esp_heap_caps.h>
// All configuration and broker callbacks run on loop(). ESP MQTT tasks only
// assemble bounded messages and enqueue them; they never read mutable JSON.
// INTERNAL alone includes instruction RAM that cannot hold byte buffers.
static size_t hubByteHeap() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static size_t hubByteBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static bool hubReload = false, hubCredentials = false, hubBirth = false;
static std::atomic<bool> hubRestoring{false};
static esp_mqtt_client_handle_t bemfaMqtt = nullptr;
static std::atomic<bool> bemfaConnected{false};
static String bemfaConfiguredKey;
static std::vector<std::string> hubRemoteTopics, bemfaTopics;
static std::atomic<uint32_t> hubEpoch{0};
static std::atomic<uint32_t> hubInboxDropped{0};
struct HubMessage {
  char topic[257] = {}, body[4097] = {};
  size_t length = 0, total = 0;
  unsigned source = 0;
  uint32_t epoch = 0;
  bool retain = false, receiving = false;
};
static std::unique_ptr<HubMessage[]> hubMessages;
static QueueHandle_t hubInbox = nullptr;
static String hubHex(const uint8_t *data, size_t n) {
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += "0123456789abcdef"[data[i] >> 4];
    s += "0123456789abcdef"[data[i] & 15];
  }
  return s;
}
static std::string hubRandom(size_t n) {
  if (n > 32)
    return {};
  uint8_t bytes[32];
  esp_fill_random(bytes, n);
  String hex = hubHex(bytes, n);
  memset(bytes, 0, sizeof(bytes));
  return std::string(hex.c_str());
}
static std::string hubDerive(const std::string &password,
                             const std::string &salt) {
  uint8_t digest[32];
#if MBEDTLS_VERSION_MAJOR >= 3
  bool ok = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
      (const uint8_t *)password.data(), password.size(),
      (const uint8_t *)salt.data(), salt.size(),
      10000, sizeof(digest), digest) == 0;
#else
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                             1) == 0 &&
            mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const uint8_t *)password.data(),
                                      password.size(),
                                      (const uint8_t *)salt.data(), salt.size(),
                                      10000, sizeof(digest), digest) == 0;
  mbedtls_md_free(&ctx);
#endif
  String result = ok ? hubHex(digest, sizeof(digest)) : String("");
  memset(digest, 0, sizeof(digest));
  return std::string(result.c_str());
}
static bool hubCapture(esp_mqtt_event_handle_t event, unsigned source) {
  if (hubRestoring || !hubMessages || !hubInbox || source > 1) return false;
  auto &part = hubMessages[source];
  if (event->current_data_offset == 0) {
    part.receiving = false;
    std::string topic(event->topic, event->topic_len);
    const auto &topics = source ? bemfaTopics : hubRemoteTopics;
    if (std::find(topics.begin(), topics.end(), topic) == topics.end())
      return false;
    if (event->total_data_len < 0 || event->total_data_len > 4096 ||
        event->topic_len > 256)
      return true;
    part.length = 0;
    part.total = event->total_data_len;
    part.source = source;
    part.epoch = hubEpoch;
    part.retain = event->retain;
    memcpy(part.topic, topic.data(), topic.size());
    part.topic[topic.size()] = 0;
    part.receiving = true;
  }
  if (!part.receiving)
    return false;
  if (event->current_data_offset != (int)part.length || event->data_len < 0 ||
      part.length + event->data_len > part.total) {
    part.receiving = false;
    return true;
  }
  memcpy(part.body + part.length, event->data, event->data_len);
  part.length += event->data_len;
  if (part.length == part.total) {
    part.body[part.length] = 0;
    if (xQueueSend(hubInbox, &part, 0)!=pdTRUE)
      ++hubInboxDropped;
    part.receiving = false;
  }
  return true;
}
static void hubConfigureUpstream() {
  hubRemoteTopics = hubRelay.subscriptions();
}
static void hubSubscribe() {
  for (const auto &topic : hubRemoteTopics)
    esp_mqtt_client_subscribe(mqtt, topic.c_str(), 1);
}
static void bemfaEvent(void *, esp_event_base_t, int32_t id, void *data) {
  if (hubRestoring) return;
  auto event = (esp_mqtt_event_handle_t)data;
  if (id == MQTT_EVENT_CONNECTED) {
    bemfaConnected = true;
    for (const auto &topic : bemfaTopics)
      esp_mqtt_client_subscribe(bemfaMqtt, topic.c_str(), 1);
  }
  if (id == MQTT_EVENT_DISCONNECTED)
    bemfaConnected = false;
  if (id == MQTT_EVENT_DATA)
    hubCapture(event, 1);
}
static void stopBemfa() {
  if (bemfaMqtt) {
    esp_mqtt_client_stop(bemfaMqtt);
    esp_mqtt_client_destroy(bemfaMqtt);
    bemfaMqtt = nullptr;
  }
  bemfaConnected = false;
  bemfaTopics.clear();
  bemfaConfiguredKey = "";
}
static void startBemfa() {
  stopBemfa();
  if (config["hub"]["bemfa"]["enabled"] != true) {
    hubRelay.status = "disabled";
    return;
  }
  bemfaConfiguredKey = config["hub"]["bemfa"]["api_key"] | "";
  for (JsonObjectConst map :
       config["hub"]["bemfa"]["mappings"].as<JsonArrayConst>())
    bemfaTopics.push_back(map["topic"] | "");
  esp_mqtt_client_config_t c = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  c.broker.address.hostname = "bemfa.com";
  c.broker.address.port = 9501;
  c.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  c.credentials.client_id = bemfaConfiguredKey.c_str();
  c.session.keepalive = 30;
  c.buffer.size = 1024;
  c.buffer.out_size = 1024;
#else
  c.host = "bemfa.com";
  c.port = 9501;
  c.client_id = bemfaConfiguredKey.c_str();
  c.keepalive = 30;
  c.buffer_size = 1024;
  c.out_buffer_size = 1024;
#endif
  bemfaMqtt = esp_mqtt_client_init(&c);
  hubRelay.status = bemfaMqtt ? "connecting" : "configuration_error";
  if (bemfaMqtt) {
    esp_mqtt_client_register_event(bemfaMqtt, MQTT_EVENT_ANY, bemfaEvent,
                                   nullptr);
    esp_mqtt_client_start(bemfaMqtt);
  }
}
static bool hubRestartPending() { return hubRestoring; }
static void hubResetInbox() {
  ++hubEpoch;
  if (hubInbox) xQueueReset(hubInbox);
  if (hubMessages)
    for (unsigned i = 0; i < 3; ++i) hubMessages[i] = HubMessage();
}
static void hubUpstreamChanged() {
  // config.set may run inside the local broker's poll callback. Stop the old
  // account now, then let the next reload retire both inbox generations before
  // starting a new client, including when this poll has already drained input.
  stopMqtt();
  mqttRestart = false;
  hubReload = true;
}
static void hubRestoreChanged() {
  // Called only after restore/reset has committed. Invalidate the old account
  // synchronously; no callback or next loop iteration may use its sessions.
  hubRestoring = true;
  homeQuiesce();
  hubService.clearSessions();
  hubHttpToken = "";
  hubTransport.stop();
  stopMqtt();
  stopBemfa();
  hubRelay.reset();
  hubResetInbox();
  hubRemoteTopics.clear();
  hubReload = hubCredentials = hubBirth = mqttRestart = wifiRestart = false;
}
static bool hubReady() {
  return !hubRestoring && !configRecovery && hubService.ready() && hubTransport.running();
}
static void hubApiExecute() {
  if (hubRestoring) {
    hubHttpStatus = 503;
    webResult = "{\"code\":\"restart_required\"}";
    hubHttpToken = "";
    return;
  }
  if (configRecovery) {
    hubHttpStatus = 409;
    webResult = "{\"code\":\"migration_required\"}";
    hubHttpToken = "";
    return;
  }
  if (hubByteHeap() < hubService.workingMemory(hubHttpRequest) ||
      hubByteBlock() < hubService.largestAllocation(hubHttpRequest)) {
    hubHttpStatus = 503;
    webResult = "{\"code\":\"busy\"}";
    hubHttpToken = "";
    return;
  }
  auto reply = hubService.handle(hubHttpRequest, millis());
  hubHttpStatus = reply.status;
  hubHttpToken = reply.token.c_str();
  webResult = reply.body.c_str();
}
static void hubLocalPublish(const String &topic, const String &body,
                            bool retain) {
  if (!hubReady())
    return;
  // Native sensor/HA messages are materialized independently for the local
  // account. The upstream account namespace never becomes a local credential.
  String upstreamKey = mqttRoot.substring(11, mqttRoot.indexOf("/daikin/")),
         localKey = iotcloud::HubService::userKey(hubService.user()).c_str();
  String target = topic, payload = body;
  target.replace("phicomm/v2/" + upstreamKey + "/",
                 "phicomm/v2/" + localKey + "/");
  target.replace("homeassistant/sensor/" + upstreamKey + "/",
                 "homeassistant/sensor/" + localKey + "/");
  payload.replace("phicomm/v2/" + upstreamKey + "/",
                  "phicomm/v2/" + localKey + "/");
  if (topic.startsWith("homeassistant/"))
    payload.replace(upstreamKey + "_daikin_", localKey + "_daikin_");
  hubTransport.broker.publish(target.c_str(), payload.c_str(), retain,
                              iotcloud::HubBroker::Origin::Internal);
}
static void hubInit() {
  hubMessages.reset(new (std::nothrow) HubMessage[3]);
  hubInbox = xQueueCreate(3, sizeof(HubMessage));
  hubService.homeStatus = homeStatus;
  hubService.homeReset = homeReset;
  hubHome.randomHex = hubRandom;
  hubService.randomHex = hubRandom;
  hubService.derive = hubDerive;
  hubService.store = saveConfig;
  hubService.changed = [](bool credentials) {
    hubReload = true;
    hubCredentials |= credentials;
  };
  hubService.bemfaStatus = []() { return hubRelay.status; };
  hubTransport.admission = []() {
    return hubByteHeap() > 32768 && hubByteBlock() > 12288;
  };
  hubTransport.websocketAccept = [](const std::string &key) {
    auto input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t hash[20];
    char result[32];
    size_t n = 0;
#if MBEDTLS_VERSION_MAJOR >= 3
    int hashResult = mbedtls_sha1((const uint8_t *)input.data(), input.size(), hash);
#else
    int hashResult = mbedtls_sha1_ret((const uint8_t *)input.data(), input.size(), hash);
#endif
    if (hashResult ||
        mbedtls_base64_encode((uint8_t *)result, sizeof(result), &n, hash,
                              sizeof(hash)))
      return std::string();
    return std::string(result, n);
  };
  hubTransport.broker.allocate = [](size_t n) {
    return hubByteHeap() > n + 16384 && hubByteBlock() > n;
  };
  hubTransport.broker.authenticate = [](const std::string &u,
                                        const std::string &p) {
    return !configRecovery && hubService.authenticate(u, p, millis());
  };
  hubTransport.broker.authorize = [](const std::string &t, bool write) {
    return hubService.allowed(t, write);
  };
  hubTransport.broker.event = [](const std::string &topic,
                                 const std::string &body, bool retain,
                                 iotcloud::HubBroker::Origin origin) {
    if(origin==iotcloud::HubBroker::Origin::Local || origin==iotcloud::HubBroker::Origin::Internal){hubHome.tick(millis());hubHome.observe(topic,body);}
    hubRelay.local(topic, body, retain, origin);
    if (topic == "homeassistant/status" && body == "online")
      haBirth = true;
    auto base = hubService.prefix() + "daikin/" + std::string(deviceId.c_str());
    auto sensor =
        hubService.prefix() + "sensor/" + std::string(deviceId.c_str());
    if (!retain &&
        (topic == base + "/command" || topic == sensor + "/command")) {
      if (otaActive)
        return;
      if (!sessionGrant(body.c_str()) && topic == base + "/command")
        hubTransport.broker.publish(base + "/result",
                                    execute(body.c_str()).c_str(), false,
                                    iotcloud::HubBroker::Origin::Internal);
    }
  };
  hubRelay.randomHex = hubRandom;
  hubRelay.upstream = [](const std::string &t, const std::string &b,
                         bool retain) {
    return mqtt && mqttConnected &&
           esp_mqtt_client_publish(mqtt, t.c_str(), b.c_str(), b.size(), 1,
                                   retain) >= 0;
  };
  hubRelay.bemfa = [](const std::string &t, const std::string &b, bool) {
    return bemfaMqtt && bemfaConnected &&
           esp_mqtt_client_publish(bemfaMqtt, t.c_str(), b.c_str(), b.size(), 1,
                                   false) >= 0;
  };
  hubReload = true;
}
static void pollHub(uint32_t now) {
  if (hubRestoring) return;
  if (hubReload) {
    hubReload = false;
    homeChanged();
    if (hubCredentials) {
      hubCredentials = false;
      hubTransport.broker.reset();
      hubBirth = hubTransport.running();
    }
    if (!configRecovery && hubService.ready() && !hubTransport.running()) {
      hubBirth = hubTransport.start();
    }
    // SDK callbacks copy the current epoch into each queued message. Join both
    // old client tasks before publishing a new epoch or changing their topics.
    stopMqtt();
    stopBemfa();
    hubRelay.reset();
    hubResetInbox();
    mqttRestart = true;
    startBemfa();
  }
  hubRelay.tick(now);
  if (hubMessages && hubInbox) {
  auto &hubReceived = hubMessages[2];
  if (xQueueReceive(hubInbox, &hubReceived, 0) == pdTRUE &&
      hubReceived.epoch == hubEpoch && !otaActive) {
    if (hubReceived.source)
      hubRelay.incomingBemfa(hubReceived.topic,
                             std::string(hubReceived.body, hubReceived.length),
                             hubReceived.retain);
    else
      hubRelay.incoming(hubReceived.topic,
                        std::string(hubReceived.body, hubReceived.length),
                        hubReceived.retain);
  }
  }
  if (bemfaConnected &&
      (hubRelay.status == "connecting" || hubRelay.status == "disconnected"))
    hubRelay.status = "connected";
  else if (!bemfaConnected && hubRelay.status == "connected")
    hubRelay.status = "disconnected";
  hubTransport.poll(now);
  if (hubBirth) {
    hubBirth = false;
    mqttOnline = true;
    haBirth = true;
    nextPublish = now;
  }
}
