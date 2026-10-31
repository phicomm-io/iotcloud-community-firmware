/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <esp_heap_caps.h>
// All configuration and device bus callbacks run on loop(). ESP MQTT tasks only
// assemble bounded messages and enqueue them; they never read mutable JSON.
// INTERNAL alone includes instruction RAM that cannot hold byte buffers.
static size_t hubByteHeap() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static size_t hubByteBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static bool hubAllocationReady(size_t n) {
  return n <= size_t(-1) - 16384 && hubByteHeap() > n + 16384 &&
         hubByteBlock() > n;
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
// Each SDK task owns its source's partial message. The by-value inbox keeps
// completed messages; idle sources do not need permanent working buffers.
static std::unique_ptr<HubMessage> hubMessages[2];
static QueueHandle_t hubInbox = nullptr;
static std::string hubRandom(size_t n) {
  if (n > 32) return {};
  uint8_t bytes[32];esp_fill_random(bytes,n);
  std::string text;text.reserve(n*2);
  for(size_t i=0;i<n;++i){text += "0123456789abcdef"[bytes[i]>>4];text += "0123456789abcdef"[bytes[i]&15];}
  memset(bytes,0,sizeof(bytes));return text;
}
static bool hubCapture(esp_mqtt_event_handle_t event, unsigned source) {
  if (!event || hubRestoring || !hubInbox || source > 1) return false;
  auto &message = hubMessages[source];
  if (event->current_data_offset == 0) {
    message.reset();
    if (event->topic_len < 0 || (event->topic_len && !event->topic))
      return false;
    const auto &topics = source ? bemfaTopics : hubRemoteTopics;
    const auto topic = std::find_if(topics.begin(), topics.end(),
        [&](const std::string &candidate) {
          return candidate.size() == size_t(event->topic_len) &&
                 (!event->topic_len ||
                  !memcmp(candidate.data(), event->topic, event->topic_len));
        });
    if (topic == topics.end())
      return false;
    if (event->total_data_len < 0 || event->total_data_len > 4096 ||
        event->topic_len > 256 || event->data_len < 0 ||
        event->data_len > event->total_data_len ||
        (event->data_len && !event->data))
      return true;
    if (!hubAllocationReady(sizeof(HubMessage))) {
      ++hubInboxDropped;
      return true;
    }
    message.reset(new (std::nothrow) HubMessage);
    if (!message) {
      ++hubInboxDropped;
      return true;
    }
    auto &part = *message;
    part.length = 0;
    part.total = event->total_data_len;
    part.source = source;
    part.epoch = hubEpoch;
    part.retain = event->retain;
    memcpy(part.topic, topic->data(), topic->size());
    part.topic[topic->size()] = 0;
    part.receiving = true;
  }
  if (!message)
    return false;
  auto &part = *message;
  if (event->current_data_offset != (int)part.length || event->data_len < 0 ||
      size_t(event->data_len) > part.total - part.length ||
      (event->data_len && !event->data)) {
    message.reset();
    return true;
  }
  if (event->data_len)
    memcpy(part.body + part.length, event->data, event->data_len);
  part.length += event->data_len;
  if (part.length == part.total) {
    part.body[part.length] = 0;
    // Yield the SDK receive task briefly so loop() can drain the fixed inbox.
    // A zero-wait send drops ordinary four-command bursts before the owner
    // loop can return a correlated busy result. This never blocks loop() or
    // grows the inbox; persistent overload still increments the drop counter.
    if (xQueueSend(hubInbox, &part, pdMS_TO_TICKS(50))!=pdTRUE)
      ++hubInboxDropped;
    message.reset();
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
  // Both SDK clients are stopped/joined before their source slots are reset.
  ++hubEpoch;
  if (hubInbox) xQueueReset(hubInbox);
  for (auto &message : hubMessages) message.reset();
}
static void hubUpstreamChanged() {
  // config.set may run inside a local UDP command callback. Stop the old
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
  hubHttpCommand.cancel("restart_required");
  hubDeviceBus.dispatch = nullptr;
  hubLanReset();
  stopMqtt();
  stopBemfa();
  hubRelay.reset();
  hubResetInbox();
  hubRemoteTopics.clear();
  hubReload = hubCredentials = hubBirth = mqttRestart = wifiRestart = false;
}
static bool hubReady() {
  return !hubRestoring && !configRecovery && hubService.ready() && bool(hubDeviceBus.dispatch);
}
static void hubApiExecute() {
  if (hubRestoring) {
    hubHttpStatus = 503;
    webResult = "{\"code\":\"restart_required\"}";
    return;
  }
  if (configRecovery) {
    hubHttpStatus = 409;
    webResult = "{\"code\":\"migration_required\"}";
    return;
  }
  if(hubHttpCommandStart())return;
  if (hubByteHeap() < hubService.workingMemory(hubHttpRequest) ||
      hubByteBlock() < hubService.largestAllocation(hubHttpRequest)) {
    hubHttpStatus = 503;
    webResult = "{\"code\":\"busy\"}";
    return;
  }
  auto reply = hubService.handle(hubHttpRequest, millis());
  hubHttpStatus = reply.status;
  webResult = reply.body.c_str();
}
static void hubLocalPublish(const String &topic, const String &body,
                            bool retain) {
  // Admit namespace scratch before constructing any owning string. The byte
  // reserve in device bus dispatch cannot protect allocations made by its caller.
  if (!topic || !body || topic.length() > iotcloud::HubDeviceBus::Topic ||
      body.length() > iotcloud::HubDeviceBus::Packet ||
      !hubReady() || !hubAllocationReady(2048))
    return;
  // Native sensor/HA messages are materialized independently for the local
  // account. The upstream account namespace never becomes a local credential.
  const int boundary = mqttRoot.indexOf("/daikin/");
  if (!mqttRoot || boundary < 11 || boundary > 139) return;
  const std::string upstreamKey(mqttRoot.c_str() + 11, size_t(boundary - 11));
  const std::string localKey = iotcloud::HubService::userKey(hubService.user());
  const std::string from = "phicomm/v2/" + upstreamKey + "/",
                    to = "phicomm/v2/" + localKey + "/";
  const std::string haFrom = "homeassistant/sensor/" + upstreamKey + "/",
                    haTo = "homeassistant/sensor/" + localKey + "/";
  const std::string idFrom = upstreamKey + "_daikin_",
                    idTo = localKey + "_daikin_";
  const iotcloud::NativeRule topicRules[] = {{from, to}, {haFrom, haTo}};
  const iotcloud::NativeRule bodyRules[] = {{from, to}, {idFrom, idTo}};
  size_t targetLength = 0, payloadLength = 0;
  if (!iotcloud::nativeMessageSize(topic.c_str(), topic.length(), topicRules, 2,
                                  iotcloud::HubDeviceBus::Topic, targetLength) ||
      !iotcloud::nativeMessageSize(body.c_str(), body.length(), bodyRules,
                                  topic.startsWith("homeassistant/") ? 2 : 0,
                                  iotcloud::HubDeviceBus::Packet, payloadLength) ||
      targetLength + payloadLength + 4 > iotcloud::HubDeviceBus::Packet ||
      !hubAllocationReady(2 * (targetLength + payloadLength) + 2048)) return;
  std::string target, payload;
  target.reserve(targetLength); payload.reserve(payloadLength);
  iotcloud::nativeMessageAppend(target, topic.c_str(), topic.length(), topicRules, 2);
  iotcloud::nativeMessageAppend(payload, body.c_str(), body.length(), bodyRules,
                                topic.startsWith("homeassistant/") ? 2 : 0);
  hubDeviceBus.publish(target, payload, retain,
                              iotcloud::HubDeviceBus::Origin::Internal);
}
static void hubInit() {
  hubInbox = xQueueCreate(3, sizeof(HubMessage));
  hubService.localMac = deviceId.c_str();
  hubService.homeStatus = homeStatus;
  hubService.lanStatus = hubLanStatus;
  hubService.deviceMetadata = hubDeviceMetadata;
  hubService.homeReset = homeReset;
  hubHome.randomHex = hubRandom;
  hubService.randomHex = hubRandom;
  hubService.store = saveConfig;
  hubService.changed = [](bool credentials) {
    hubReload = true;
    hubCredentials |= credentials;
  };
  hubService.bemfaStatus = []() { return hubRelay.status; };
  if(!configRecovery&&!hubService.initializeIdentity())configRecovery=true;
  hubDeviceBus.dispatch = [](const std::string &topic,const std::string &body,
                              bool retain,iotcloud::HubDeviceBus::Origin origin) {
    if(hubRestoring || configRecovery)return false;
    const bool local=origin==iotcloud::HubDeviceBus::Origin::Local || origin==iotcloud::HubDeviceBus::Origin::Internal;
    if(local){
      iotcloud::hubWire.observe(topic,body);
      hubHome.tick(millis());hubHome.observe(topic,body);
      hubRelay.local(topic,body,retain,origin);
    }
    const auto base=hubService.prefix()+"daikin/"+std::string(deviceId.c_str());
    const auto sensor=hubService.prefix()+"sensor/"+std::string(deviceId.c_str());
    if(topic==base+"/command" || topic==sensor+"/command"){
      if(retain || otaActive)return false;
      if(!sessionGrant(String(body.data(),body.size())) && topic==base+"/command"){
        const String result=execute(String(body.data(),body.size()));
        return hubDeviceBus.publish(base+"/result",std::string(result.c_str(),result.length()),false,iotcloud::HubDeviceBus::Origin::Internal);
      }
      return true;
    }
    if(topic.size()>=8&&topic.compare(topic.size()-8,8,"/command")==0)return hubLanSend(topic,body,retain);
    return local;
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
    hubLanReset();
    if(hubCredentials)hubCredentials=false;
    hubBirth=!configRecovery && hubService.ready();
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
  if (hubInbox && uxQueueMessagesWaiting(hubInbox) &&
      hubAllocationReady(sizeof(HubMessage))) {
    // Do not dequeue until the temporary copy exists. Allocation pressure can
    // retry next poll without dropping the queued value or using a large stack.
    std::unique_ptr<HubMessage> hubReceived(new (std::nothrow) HubMessage);
    if (hubReceived && xQueueReceive(hubInbox, hubReceived.get(), 0) == pdTRUE &&
        hubReceived->epoch == hubEpoch && !otaActive) {
      if (hubReceived->source)
        hubRelay.incomingBemfa(hubReceived->topic,
                              std::string(hubReceived->body, hubReceived->length),
                              hubReceived->retain);
      else
        hubRelay.incoming(hubReceived->topic,
                          std::string(hubReceived->body, hubReceived->length),
                          hubReceived->retain);
    }
  }
  if (bemfaConnected &&
      (hubRelay.status == "connecting" || hubRelay.status == "disconnected"))
    hubRelay.status = "connected";
  else if (!bemfaConnected && hubRelay.status == "connected")
    hubRelay.status = "disconnected";
  pollHubLan(now);
  if (hubBirth) {
    hubBirth = false;
    mqttOnline = true;
    haBirth = true;
    nextPublish = now;
  }
}
