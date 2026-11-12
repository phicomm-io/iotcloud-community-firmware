/* SPDX-License-Identifier: GPL-3.0-or-later
 * Board adapter. Included in the existing DAIKIN application, so HTTPD,
 * configuration, authenticated LAN, MQTT queues and management UI are shared.
 * All factory topics/JSON terminate here; none are forwarded to the network.
 */
#include "stock_frame.h"
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <time.h>
#include "screen_clock.inc"
static void screenMediaInit();
static void screenMediaPoll(uint32_t);
static bool screenMediaFeed(unsigned,uint32_t);
static bool screenMediaAnnounce(const String &);
static bool screenMediaNegotiating();
static void screenServicesStatus(JsonObject);
static void screenServicesInit();
static void screenServicesPoll(uint32_t);
static bool screenOtaCommand(const String &, const String &);
static bool screenOtaActive();
static void screenOtaReplyError(const String &, unsigned);
static bool screenBusinessSend(const char *, size_t);
static bool screenGuardLine(const uint8_t *, size_t);
static bool screenGuardAnnounce(const String &);
static bool screenGuardImageAnnounce(const String &);
static bool screenExtendedLine(const uint8_t *, size_t);
static String screenHex(const uint8_t *, size_t);
static const char *screenApplySettings(JsonObjectConst, JsonObject);
static void screenGetSettings(JsonObject);
static bool screenCalibrationResponse(const String &, const String &, int);
static bool screenWeatherStockRequest(const String &);
static HardwareSerial screenUart(1);
static esp_pm_lock_handle_t screenWakeLock, screenTxLock;
static bool screenWakeHeld=false, screenTxHeld=false;
static uint32_t screenTxAt=0;
// All UART writers run in the application owner, including binary MCU OTA.
static size_t screenUartWrite(const uint8_t *bytes,size_t n) {
  if(!screenTxHeld){ESP_ERROR_CHECK(esp_pm_lock_acquire(screenTxLock));screenTxHeld=true;}
  size_t written=screenUart.write(bytes,n);
  screenTxAt=millis();
  return written;
}
static void screenPowerPoll(uint32_t now,bool updating) {
  bool awake=digitalRead(2)==HIGH||updating;
  if(awake!=screenWakeHeld){
    ESP_ERROR_CHECK(awake?esp_pm_lock_acquire(screenWakeLock):esp_pm_lock_release(screenWakeLock));
    screenWakeHeld=awake;
  }
  // Keep the factory response grace period and wait for physical TX to drain.
  if(screenTxHeld&&uint32_t(now-screenTxAt)>=50&&uart_wait_tx_done(UART_NUM_1,0)==ESP_OK){
    ESP_ERROR_CHECK(esp_pm_lock_release(screenTxLock));screenTxHeld=false;
  }
}
static bry::StockFrame stockFrame;
static DynamicJsonDocument screenSnapshot(4096);
static uint32_t screenLastRx = 0, screenFrames = 0, screenDataAt = 0,
                screenNextSnapshot = 0, screenSnapshotAt = 0;
static String screenMcuVersion, screenGalleryVersion, screenGateway;
static bool screenSession = false, screenProvisioning = false;
static uint8_t screenRaw[64];
static size_t screenRawSize = 0;
struct ScreenMessage {
  String wire;
  uint32_t at = 0;
};
static ScreenMessage screenQueue[12];
static unsigned screenHead = 0, screenTail = 0;
static String screenPendingToken;
static uint32_t screenPendingAt = 0;
static int screenPendingCode = -1;
static bool screenSend(const String &line, uint32_t delayMs = 0) {
  if (line.length() > 3072)
    return false;
  if (!delayMs)
    return screenUartWrite((const uint8_t *)line.c_str(), line.length()) ==
           line.length();
  unsigned next = (screenTail + 1) % 12;
  if (next == screenHead)
    return false;
  screenQueue[screenTail].wire = line;
  screenQueue[screenTail].at = millis() + delayMs;
  screenTail = next;
  return true;
}
static void screenReply(const String &cmd, bool ok = true, unsigned error = 1) {
  if (!ok && screenOtaActive()) screenOtaReplyError(cmd, error);
  char suffix[32];
  if (ok)
    snprintf(suffix, sizeof(suffix), ":OK\r\n");
  else
    snprintf(suffix, sizeof(suffix), ":ERROR<%04X>\r\n", error);
  screenSend("+" + cmd + suffix);
}
static String screenRoot() {
  String mac = deviceId;
  mac.toUpperCase();
  return "SENSOR/" + mac;
}
static bool screenBuildDelivery(const String &topic, JsonDocument &doc,
                                String &wire) {
  if (screenOtaActive() && !topic.endsWith("/service/ota"))
    return false;
  const size_t body = measureJson(doc);
  char length[16];
  snprintf(length, sizeof(length), "%u", (unsigned)body);
  size_t size = 10 + topic.length() + strlen(length) + body + 4;
  if (doc.overflowed() || !body || size > 3072 || !wire.reserve(size))
    return false;
  wire = "";
  if (!wire.concat("+sub_recv:") || !wire.concat(topic) || !wire.concat(",") ||
      !wire.concat(length) || !wire.concat(","))
    return false;
  return serializeJson(doc, wire) == body && wire.concat("\r\n") &&
         wire.length() == size;
}
static bool screenDelivery(const String &topic, JsonDocument &doc,
                           uint32_t wait = 1500) {
  String wire;
  return screenBuildDelivery(topic, doc, wire) && screenSend(wire, wait);
}
static const char *screenSettingsRequestState() {
  if (!screenPendingToken.length())
    return "idle";
  if (screenPendingCode == 0)
    return "confirmed";
  if (screenPendingCode > 0)
    return "rejected";
  return uint32_t(millis() - screenPendingAt) < 10000 ? "pending" : "timeout";
}
static void screenRequestSnapshot() {
  DynamicJsonDocument doc(256);
  doc["client_token"] = "iotcloud-snapshot-" + String(millis());
  doc.createNestedObject("data");
  screenDelivery(screenRoot() + "/mqtt/service/get_snapshot", doc, 0);
}
static void screenPowerStatus(JsonObject pm) {
  esp_pm_config_esp32_t power = {};
  ESP_ERROR_CHECK(esp_pm_get_configuration(&power));
  pm["max_cpu_mhz"] = power.max_freq_mhz;
  pm["min_cpu_mhz"] = power.min_freq_mhz;
  pm["automatic_light_sleep"] = power.light_sleep_enable;
  pm["mcu_wake_high"] = digitalRead(2)==HIGH;
  pm["mcu_wake_lock"] = screenWakeHeld;
  pm["uart_tx_lock"] = screenTxHeld;
  pm["last_wakeup_cause"] = unsigned(esp_sleep_get_wakeup_cause());
}
static void screenStatus(JsonObject out) {
  out["hardware_model"] = "BRY88BBA15G";
  out["cpu_frequency_mhz"] = getCpuFrequencyMhz();
  screenPowerStatus(out.createNestedObject("power_management"));
  out["sensor_source"] = "screen_mcu";
  out["co2_warmup_seconds_remaining"] = nullptr;
  out.remove("co2_diagnostics");
  out.remove("climate_compensation");
  out["indicator_rules_supported"] = false;
  out["button_pressed"] = nullptr;
  out.remove("indicator_rules");
  out.remove("indicator_modes");
  out.remove("indicator_rules_custom");
  out.remove("system_indicator");
  JsonObject s = out.createNestedObject("screen");
  s["mcu_version"] = screenMcuVersion;
  s["gallery_version"] = screenGalleryVersion;
  s["uart_frames"] = screenFrames;
  s["last_rx_age_ms"] = screenLastRx ? millis() - screenLastRx : 0;
  s["business_session"] = screenSession ? "local_adapter" : "offline";
  s["upstream_connected"] = mqttConnected;
  s["ble_provisioning_supported"] = false;
  s["gateway_binding"] = "local_compatibility_only";
  s["weather_available"] = false;
  screenServicesStatus(s);
  s["settings"] = screenSnapshot["setting"];
  s["settings_request"]["token"] = screenPendingToken;
  s["settings_request"]["code"] = screenPendingCode;
  s["settings_request"]["state"] = screenSettingsRequestState();
  if (screenRawSize) {
    JsonArray raw = s.createNestedArray("raw_payload");
    for (size_t i = 0; i < screenRawSize; i++)
      raw.add(screenRaw[i]);
    s["raw_age_ms"] = millis() - screenDataAt;
  }
  if (screenSnapshot.containsKey("status"))
    s["status"] = screenSnapshot["status"];
}
static bool screenTime(const char *s) {
  return s && strlen(s) == 5 && s[2] == ':' && s[0] >= '0' && s[0] <= '2' &&
         s[1] >= '0' && s[1] <= '9' && (s[0] != '2' || s[1] <= '3') &&
         s[3] >= '0' && s[3] <= '5' && s[4] >= '0' && s[4] <= '9';
}
static const char *screenSettings(const String &action, JsonObjectConst params,
                                  JsonObject data) {
  if (action == "screen.settings.get") {
    if (params.size())
      return "invalid_params";
    screenGetSettings(data);
    return "ok";
  }
  if (params.size() != 1)
    return "invalid_params";
  if (screenPendingToken.length() && screenPendingCode < 0 &&
      millis() - screenPendingAt < 10000)
    return "busy";
  bool valid = false;
  if (params.containsKey("display")) {
    JsonObjectConst d = params["display"];
    valid = d.size() == 4;
    for (auto k :
         {"luminance", "data_type", "power_screen_time", "battery_screen_time"})
      valid = valid && d[k].is<unsigned>();
    valid = valid && d["luminance"].as<unsigned>() <= 100 &&
            d["data_type"].as<unsigned>() <= 1 &&
            d["power_screen_time"].as<unsigned>() <= 2 &&
            d["battery_screen_time"].as<unsigned>() <= 2;
  } else if (params.containsKey("no_disturb")) {
    JsonObjectConst d = params["no_disturb"];
    valid = d.size() == 3 && d["state"].is<bool>() &&
            screenTime(d["start_tm"] | "") && screenTime(d["end_tm"] | "");
  } else if (params.containsKey("ipbox_push"))
    valid = params["ipbox_push"].is<bool>();
  // Unknown warning scaling/ranges and calibration are deliberately
  // unsupported.
  if (!valid)
    return "invalid_params";
  return screenApplySettings(params, data);
}
static void screenMeasurements(JsonVariantConst status) {
  if (!status.is<JsonObjectConst>())
    return;
  const char *fields[] = {"pm25", "co2", "temp", "humidity", "tvoc", "hcho"};
  const char *states[] = {"pm25_status",     "co2_status",  "temp_status",
                          "humidity_status", "tvoc_status", "hcho_status"};
  // JSON snapshots have already been scaled by the original MCU's formatter.
  // Keep raw status exposed; only the recovered normal status (0x51) is valid.
  for (unsigned i = 0; i < 6; i++) {
    if (i == TVOC) {
      // Matching Cubic UART 0x16 protocol: BE16 DF1/DF2 is already ppb.
      // The factory MCU copies it to `tvoc` with a 2000 ppb cap. `tvoc_level`
      // is a separate display grade and must never become the concentration.
      unsigned ppb = status["tvoc"] | 0u;
      if (status["tvoc_status"].is<unsigned>() &&
          status["tvoc_status"].as<unsigned>() == 0x51 &&
          status["tvoc"].is<unsigned>() && ppb <= 2000)
        readings[i].set(ppb, millis());
      else
        readings[i].received = false;
      continue;
    }
    if (!status[states[i]].is<unsigned>() ||
        status[states[i]].as<unsigned>() != 0x51 ||
        !status[fields[i]].is<float>()) {
      readings[i].received = false;
      continue;
    }
    float value = status[fields[i]].as<float>();
    if (!isfinite(value)) {
      readings[i].received = false;
      continue;
    }
    if (i == TEMPERATURE)
      value /= 10.0f;
    // HCHO raw scaling is not yet independently verified on the old MCU.
    if (i == HCHO) {
      readings[i].received = false;
      continue;
    }
    readings[i].set(value, millis());
  }
  hchoPresent = status.containsKey("hcho_status") &&
                status["hcho_status"].as<unsigned>() != 0x59;
}
static bool screenPublication(const String &topic, const uint8_t *payload,
                              size_t size) {
  if (!topic.startsWith(screenRoot() + "/") &&
      !topic.startsWith("IPBOX/" + screenGateway + "/" + screenRoot() + "/"))
    return false;
  if (topic.endsWith("/upload_data")) {
    if (size != 20)
      return false;
    memcpy(screenRaw, payload, size);
    screenRawSize = size;
    screenDataAt = millis();
    invalidateBusinessStatusCache();
    return true;
  }
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, payload, size) || !doc.is<JsonObject>())
    return false;
  String token = doc["client_token"] | "";
  if (!token.length() || token.length() > 128)
    return false;
  if (screenCalibrationResponse(topic, token, doc["code"] | -1)) return true;
  if (topic.endsWith("/service_response/set")) {
    if (token == screenPendingToken) {
      screenPendingCode = doc["code"] | -1;
      invalidateBusinessStatusCache();
      screenRequestSnapshot();
    }
    return true;
  }
  if (topic.endsWith("/upload_snapshot") ||
      topic.endsWith("/service_response/get_snapshot")) {
    JsonVariantConst d = doc["data"];
    String mac = d["physics"]["mac"] | "";
    String expected = deviceId;
    expected.toUpperCase();
    if (mac != expected)
      return false;
    screenClockSnapshot(doc["timestamp"], millis());
    screenSnapshot.set(d);
    screenSnapshotAt = millis();
    screenMcuVersion = d["physics"]["mcu_version"].as<String>();
    screenGalleryVersion = d["physics"]["gallery_version"].as<String>();
    screenMeasurements(d["status"]);
    invalidateBusinessStatusCache();
    if (topic.endsWith("/service_response/get_snapshot"))
      return true;
  }
  if (topic.endsWith("/get_weather"))
    return screenWeatherStockRequest(token);
  DynamicJsonDocument reply(1024);
  reply["code"] = 0;
  reply["client_token"] = token;
  reply["timestamp"] = (int64_t)time(nullptr) * 1000;
  JsonObject data = reply.createNestedObject("data");
  String route;
  if (topic.endsWith("/upload_snapshot"))
    route = screenRoot() + "/mqtt/event_response/snapshot_upload";
  else if (topic.endsWith("/login_ipbox")) {
    // Acknowledge only this local compatibility session, not a remote HVAC
    // bind.
    route = screenRoot() + "/IPBOX/" + screenGateway +
            "/mqtt/event_response/login_ipbox";
  } else if (topic.endsWith("/get_filter_sensor3")) {
    data.createNestedArray("filter_infos");
    route = topic;
    route.replace("/service/", "/service_response/");
  } else if (topic.endsWith("/get_error_sensor3")) {
    data.createNestedArray("error_infos");
    route = topic;
    route.replace("/service/", "/service_response/");
  } else if (topic.indexOf("/service_response/") >= 0)
    return true;
  else
    return false;
  return screenDelivery(route, reply);
}
static bool screenNumber(const uint8_t *p, size_t n, size_t &pos,
                         size_t &value) {
  value = 0;
  bool digit = false;
  while (pos < n && p[pos] >= '0' && p[pos] <= '9') {
    digit = true;
    value = value * 10 + p[pos++] - '0';
    if (value > 4096)
      return false;
  }
  return digit;
}
static void screenCommand(const uint8_t *p, size_t n, size_t payloadAt,
                          size_t payloadSize) {
  screenLastRx = millis();
  ++screenFrames;
  if (screenGuardLine(p, n))
    return;
  if (screenExtendedLine(p, n))
    return;
  size_t end = 0;
  while (end < n && p[end] != ' ')
    ++end;
  String cmd((const char *)p, end);
  if (cmd == "pub") {
    // Raw OTA has exclusive UART ownership. A publication already buffered
    // before the handoff must not insert either an ACK or an error reply.
    if (screenOtaActive()) return;
    size_t topicEnd = 4;
    while (topicEnd < n && p[topicEnd] != ' ')
      ++topicEnd;
    String topic((const char *)p + 4, topicEnd - 4);
    bool ok = screenSession && payloadAt &&
              screenPublication(topic, p + payloadAt, payloadSize);
    screenReply("pub", ok, ok ? 0 : 0x43);
    return;
  }
  String arg =
      end < n ? String((const char *)p + end + 1, n - end - 1) : String();
  if (cmd == "screen_media") { screenMediaAnnounce(arg); return; }
  if (screenOtaCommand(cmd, arg))
    return;
  // Damaged/stale media or business lines are not OTA commands. In particular,
  // replying ERROR here would make the maintenance receiver cancel its image.
  if (screenOtaActive()) return;
  // Only command names are logged. Never echo Wi-Fi/password/JSON arguments.
  bool logName = cmd.length() > 0 && cmd.length() <= 32;
  for (size_t i = 0; i < cmd.length(); ++i)
    logName = logName && ((cmd[i] >= 'a' && cmd[i] <= 'z') || cmd[i] == '_' ||
                          cmd[i] == '?');
  if (logName)
    Serial.printf("screen command %s\n", cmd.c_str());
  else
    Serial.println("screen unrecognized frame");
  if (cmd == "mac?") {
    String mac = deviceId;
    mac.toUpperCase();
    screenSend("+mac:" + mac + "\r\n");
  } else if (cmd == "version?")
    screenSend("+version:" + String(BRY_UART_VERSION) + "\r\n");
  else if (cmd == "gateway_mac?") {
    if (screenGateway.length() == 12)
      screenSend("+gateway_mac:" + screenGateway + "\r\n");
    else
      screenReply("gateway_mac");
  } else if (cmd == "wifi_state?")
    screenSend("+wifi_state:" + String(WiFi.status() == WL_CONNECTED ? 2 : 0) +
               "," + String(WiFi.RSSI()) + "\r\n");
  else if (cmd == "mqtt_user_cfg") {
    unsigned scheme = 0;
    char client[129] = {}, user[129] = {}, extra[2] = {};
    bool valid = sscanf(arg.c_str(), "%u %128s %128s %1s", &scheme, client,
                        user, extra) == 3 &&
                 scheme >= 1 && scheme <= 10;
    screenReply(cmd, valid, 0x41);
  } else if (cmd == "mqtt_conn") {
    // This opens a real in-process business adapter, not a remote MQTT session.
    // Ignore the factory host, port, username and credentials at this boundary.
    screenSession = true;
    screenReply(cmd);
    screenNextSnapshot = millis() + 15000;
  } else if (cmd == "mqtt_conn?")
    screenSend("+mqtt_conn:" + String(screenSession ? 2 : 0) +
               ",0,iotcloud-local,0,0\r\n");
  else if (cmd == "mqtt_clean") {
    screenSession = false;
    screenReply(cmd);
  } else if (cmd == "sub" || cmd == "unsub") {
    if (arg.length() > 128 || arg.indexOf('\r') >= 0)
      screenReply(cmd, false);
    else
      screenReply(cmd, screenSession, 0x63);
  } else if (cmd == "sta_con") {
    if (arg.isEmpty()) {
      if (WiFi.status() != WL_CONNECTED)
        wifiRestart = true;
      screenReply(cmd,
                  config["wifi"]["ssid"].is<const char *>() ||
                      WiFi.status() == WL_CONNECTED,
                  0x21);
    } else {
      size_t pos = end + 1, ss = 0, ps = 0;
      bool ok = screenNumber(p, n, pos, ss) && ss <= 32 && ss > 0 && pos < n &&
                p[pos++] == ' ' && ss <= n - pos;
      String ssid, password;
      if (ok) {
        ssid = String((const char *)p + pos, ss);
        pos += ss;
        ok = pos < n && p[pos++] == ' ' && screenNumber(p, n, pos, ps) &&
             ps <= 63 && pos < n && p[pos++] == ' ' && ps == n - pos;
        if (ok)
          password = String((const char *)p + pos, ps);
      }
      if (ok) {
        DynamicJsonDocument next(2048);
        next.set(config);
        next["wifi"]["ssid"] = ssid;
        next["wifi"]["password"] = password;
        ok = saveConfig(next);
        if (ok)
          wifiRestart = true;
      }
      screenReply(cmd, ok, 0x21);
    }
  } else if (cmd == "router?" || cmd == "net_cfg?") {
    String ssid = config["wifi"]["ssid"] | "",
           pass = config["wifi"]["password"] | "";
    if (ssid.isEmpty())
      screenReply(cmd.substring(0, cmd.length() - 1));
    else if (cmd == "net_cfg?")
      screenSend("+net_cfg:" + ssid + "," + pass + "\r\n");
    else
      screenSend("+router:" + String(ssid.length()) + "," + ssid + "," +
                 String(pass.length()) + "," + pass + "," +
                 String(WiFi.RSSI()) + "\r\n");
  } else if (cmd == "sntp") {
    if (time(nullptr) > 1700000000)
      screenSend("+sntp:" + String((unsigned long)time(nullptr)) + "\r\n");
    else
      screenReply(cmd, false, 0x30);
  } else if (cmd == "ble_cfg") {
    // Provisioning uses the shared management AP. BLUFI isn't advertised.
    screenProvisioning = true;
    startAccessPoint();
    screenReply(cmd, false, 0x70);
  } else if (cmd == "stop_ble_cfg") {
    screenProvisioning = false;
    screenReply(cmd);
  } else if (cmd == "ble_state?")
    screenSend("+ble_state:0\r\n");
  else if (cmd == "espnow_state?")
    screenSend("+espnow_state:0\r\n");
  else if (cmd == "clear_net_cfg") {
    DynamicJsonDocument next(2048);
    next.set(config);
    next.remove("wifi");
    bool ok = saveConfig(next);
    if (ok) {
      WiFi.disconnect(false, true);
      startAccessPoint();
    }
    screenReply(cmd, ok);
  } else if (cmd == "free")
    screenSend("+free:" + String(ESP.getFreeHeap()) + "\r\n");
  else if (cmd == "heap" || cmd == "runtime?")
    screenReply(cmd, false, 0x70);
  else if (cmd == "restart") {
    screenReply(cmd);
    rebootAt = millis() + 500;
  } else if (cmd == "bin_summary")
    screenReply(cmd, false, 0x70);
  else if (cmd == "start_ota")
    screenReply(cmd, false, 0x34);
  else if (cmd == "end_ota")
    screenReply(cmd, false, 0x35);
  else if (cmd == "download_bin" || cmd == ">")
    screenReply(cmd, false, 0x70);
  else if (cmd == "esp_ota_state?")
    screenSend("+esp_ota_state:0\r\n");
  else
    screenReply(cmd, false, 0x70);
}
static void screenInit() {
  screenUart.setRxBufferSize(8192);
  screenUart.setTxBufferSize(2048);
  screenUart.begin(115200, SERIAL_8N1, 17, 16);
  uart_config_t serial = {};
  serial.baud_rate=115200;serial.data_bits=UART_DATA_8_BITS;
  serial.parity=UART_PARITY_DISABLE;serial.stop_bits=UART_STOP_BITS_1;
  serial.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;serial.source_clk=UART_SCLK_REF_TICK;
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_1,&serial));
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX,0,"sensor3",&screenWakeLock));
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX,0,"uart_resp",&screenTxLock));
  pinMode(2, INPUT_PULLDOWN); // Original MCU wake line; never drive it.
  ESP_ERROR_CHECK(gpio_sleep_sel_dis(GPIO_NUM_2));
  ESP_ERROR_CHECK(gpio_wakeup_enable(GPIO_NUM_2,GPIO_INTR_HIGH_LEVEL));
  ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
  pmModel = "screen-mcu";
  // A management OTA resets only ESP32. The MCU can retain its connected state
  // and publish immediately without repeating mqtt_conn. The in-process owner
  // is already available; make that local session ready from boot.
  screenSession = true;
  screenNextSnapshot = millis() + 15000;
  // Import the original station configuration without erasing any namespace.
  if (!config["wifi"]["ssid"].is<const char *>()) {
    wifi_config_t saved = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK &&
        saved.sta.ssid[0]) {
      char ssid[33] = {}, pass[65] = {};
      memcpy(ssid, saved.sta.ssid, 32);
      memcpy(pass, saved.sta.password, 64);
      DynamicJsonDocument imported(2048);
      imported.set(config);
      imported["wifi"]["ssid"] = ssid;
      imported["wifi"]["password"] = pass;
      config = std::move(imported);
    }
  }
  // The existing stock association is retained only as a local compatibility
  // ID. It is not a claim that this MCU is associated with an HVAC gateway.
  nvs_handle_t handle;
  size_t length = 6;
  uint8_t gateway[6] = {};
  if (nvs_open("sensor3", NVS_READONLY, &handle) == ESP_OK) {
    if (nvs_get_blob(handle, "ipbox_mac", gateway, &length) == ESP_OK &&
        length == 6) {
      char text[13];
      snprintf(text, sizeof(text), "%02X%02X%02X%02X%02X%02X", gateway[0],
               gateway[1], gateway[2], gateway[3], gateway[4], gateway[5]);
      screenGateway = text;
    }
    nvs_close(handle);
  }
  screenServicesInit();
  screenSend("\r\n+screen_link_reset:1\r\n");
  Serial.printf("screen UART1 RX17 TX16 initialized; firmware %s\n", version);
}
static void pollSensors(uint32_t) { /* Samples arrive from the MCU, never probe
                                       old-board I2C/UART pins. */
}
static void screenPoll(uint32_t now) {
  screenClockPoll(now);
  size_t budget = 4096;
  while (budget-- && screenUart.available()) {
    unsigned c = screenUart.read();
    // Length-delimited stock publications own all payload bytes, including NUL.
    if (stockFrame.binary || !screenMediaFeed(c,now))
      stockFrame.feed(c, screenCommand);
  }
  // RX callbacks timestamp fresh announcements; use a tick after those writes.
  now = millis();
  if (!screenOtaActive() && screenHead != screenTail &&
      due(now, screenQueue[screenHead].at)) {
    const String &wire = screenQueue[screenHead].wire;
    if (screenBusinessSend(wire.c_str(), wire.length())) {
      // Arduino String retains its heap buffer on assignment from "". A
      // null C string invalidates it, returning completed UART storage to TLS.
      screenQueue[screenHead].wire = static_cast<const char *>(nullptr);
      screenHead = (screenHead + 1) % 12;
    }
  }
  screenServicesPoll(now);
  if (screenSession && !screenOtaActive() && due(now, screenNextSnapshot)) {
    screenNextSnapshot = now + 15000;
    screenRequestSnapshot();
  }
  if (screenPendingToken.length() && screenPendingCode == -1 &&
      now - screenPendingAt >= 10000) {
    screenPendingCode = -2;
    invalidateBusinessStatusCache();
  }
}
