/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "hub_service.h"
#include "hub_wire.h"

namespace iotcloud {
// Explicit, directed routes. Neither transport reconnects nor retained replays
// create a new forwarding event. Echoes are bounded by count and age.
class HubRelay {
public:
  using Origin = HubBroker::Origin;
  using Publish =
      std::function<bool(const std::string &, const std::string &, bool)>;
  Publish upstream, bemfa;
  std::function<std::string(size_t)> randomHex;
  std::string status = "disabled";
  HubRelay(DynamicJsonDocument &config, HubService &service, HubBroker &broker)
      : config(config), service(service), broker(broker) {}
  void reset() {
    for (auto &e : echoes)
      e = Echo();
    for (auto &p : pending)
      p = Pending();
    for (auto &d : devices)
      d = Device();
    for (auto &r : reported)
      r = Report();
    status = "disabled";
  }
  void tick(uint32_t now) {
    clock = now;
    for (auto &p : pending)
      if (p.used && uint32_t(clock - p.at) > 10000) {
        p.used = false;
        status = "device_timeout";
      }
  }
  std::vector<std::string> subscriptions() const {
    std::vector<std::string> topics;
    if (config["hub"]["bridge"]["enabled"] == true)
      for (JsonObjectConst r :
           config["hub"]["bridge"]["mappings"].as<JsonArrayConst>())
        if (r["direction"] == "in")
          topics.push_back(r["remote_topic"] | "");
    return topics;
  }
  void local(const std::string &topic, const std::string &body, bool retain,
             Origin origin) {
    if (origin != Origin::Local && origin != Origin::Internal)
      return;
    updateDevice(topic, body);
    if (config["hub"]["bridge"]["enabled"] == true)
      for (JsonObjectConst r :
           config["hub"]["bridge"]["mappings"].as<JsonArrayConst>())
        if (r["direction"] == "out" && r["local_topic"] == topic && upstream) {
          std::string remote = r["remote_topic"] | "";
          bool saved = r["retain"] | false;
          if (upstream(remote, body, saved && retain))
            remember(remote, body, 0);
        }
    if (config["hub"]["bemfa"]["enabled"] != true || !bemfa)
      return;
    bool mapped = false;
    for (JsonObjectConst m :
         config["hub"]["bemfa"]["mappings"].as<JsonArrayConst>())
      if (topic == base(m["device"]) + "/state") {
        mapped = true;
        break;
      }
    if (!mapped)
      return;
    DynamicJsonDocument state(8192);
    if (hubWire.decode(state, topic.substr(0,topic.size()-6), body) || !state.is<JsonObject>())
      return;
    size_t index = 0;
    for (JsonObjectConst m :
         config["hub"]["bemfa"]["mappings"].as<JsonArrayConst>()) {
      if (topic == base(m["device"]) + "/state") {
        auto value = report(m, state.as<JsonObjectConst>());
        auto &last = reported[index];
        if (!value.empty() &&
            (value != last.body || uint32_t(clock - last.at) > 30000)) {
          std::string remote = m["topic"] | "";
          if (bemfa(remote, value, false)) {
            last.body = value;
            last.at = clock;
            remember(remote, value, 1);
          }
        }
      }
      if (++index >= reported.size())
        break;
    }
  }
  void incoming(const std::string &topic, const std::string &body,
                bool retained) {
    if (echo(topic, body, 0) || config["hub"]["bridge"]["enabled"] != true)
      return;
    for (JsonObjectConst r :
         config["hub"]["bridge"]["mappings"].as<JsonArrayConst>())
      if (r["direction"] == "in" && r["remote_topic"] == topic) {
        std::string local = r["local_topic"] | "";
        if (retained && r["allow_retained"] != true)
          return;
        broker.publish(local, body,
                       (r["retain"] | false) && !HubService::command(local),
                       Origin::Upstream);
        return;
      }
  }
  bool incomingBemfa(const std::string &topic, const std::string &body,
                     bool retained) {
    if (retained || echo(topic, body, 1) ||
        config["hub"]["bemfa"]["enabled"] != true)
      return false;
    for (JsonObjectConst m :
         config["hub"]["bemfa"]["mappings"].as<JsonArrayConst>())
      if (m["topic"] == topic) {
        auto target = base(m["device"]);
        bool online = false, freshAir = false;
        for (const auto &d : devices)
          if (d.base == target && d.online && d.seen &&
              uint32_t(clock - d.at) < 20000) {
            online = true;
            freshAir = d.freshAir;
          }
        if (!online) {
          status = "device_offline";
          return false;
        }
        Pending *slot = nullptr;
        for (auto &p : pending)
          if (!p.used) {
            slot = &p;
            break;
          }
        if (!slot) {
          status = "busy";
          return false;
        }
        DynamicJsonDocument doc(768);
        if (!command(m, body, doc, freshAir))
          return false;
        auto id = "hub_bm_" + (randomHex ? randomHex(12) : "");
        if (id.size() != 31)
          return false;
        doc["request_id"] = id;
        std::string payload;
        hubWire.encode(doc, target, payload);
        slot->used = true;
        slot->id = id;
        slot->base = target;
        slot->at = clock;
        if (!broker.publish(target + "/command", payload, false,
                            Origin::Bemfa)) {
          slot->used = false;
          status = "device_rejected";
          return false;
        }
        return true;
      }
    return false;
  }
  static bool command(JsonObjectConst map, const std::string &value,
                      DynamicJsonDocument &out, bool freshAir = false) {
    if (value != "on" && value != "off" && value.compare(0, 3, "on#"))
      return false;
    std::string model = map["device"]["model"] | "";
    unsigned channel = map["channel"];
    bool on = value != "off";
    auto params = out.createNestedObject("params");
    if (model == "tc1") {
      if (value != "on" && value != "off")
        return false;
      out["action"] = "outlet.set";
      params["index"] = channel;
      params["on"] = on;
      return true;
    }
    if (model != "a1")
      return false;
    out["action"] = channel ? "light.set" : freshAir ? "fresh_air.set" : "fan.set";
    params[channel ? "enabled" : "on"] = on;
    if (!channel)
      params["mode"] = "manual";
    if (value.compare(0, 3, "on#") == 0) {
      auto raw = value.substr(3);
      if (raw.empty() || raw.size() > 3 || raw[0] == '0')
        return false;
      unsigned n = 0;
      for (char c : raw) {
        if (c < '0' || c > '9')
          return false;
        n = n * 10 + c - '0';
      }
      if (!n || n > (channel ? 100u : 4u))
        return false;
      if (channel)
        params["brightness"] = n;
      else {
        const unsigned speeds[] = {0, 20, 50, 75, 100};
        params["speed"] = speeds[n];
      }
    }
    return true;
  }
  static std::string report(JsonObjectConst m, JsonObjectConst state) {
    std::string model = m["device"]["model"] | "";
    unsigned channel = m["channel"];
    if (model == "tc1") {
      auto v = state["socket_states"][channel];
      if (!v.is<unsigned>() || v.as<unsigned>() > 1)
        return {};
      return v == 1 ? "on" : "off";
    }
    if (model != "a1")
      return {};
    bool fresh = !channel && (state.containsKey("fresh_air_on") ||
                              state.containsKey("fresh_air_speed"));
    auto enabled = state[channel ? "led_visible" : fresh ? "fresh_air_on" : "fan_on"],
         level = state[channel ? "brightness" : fresh ? "fresh_air_speed" : "percentage"];
    if (!enabled.is<bool>() || !level.is<float>())
      return {};
    double n = level.as<double>();
    if (n < 0 || n > (channel ? 255 : 100))
      return {};
    if (!enabled.as<bool>() || n == 0)
      return "off";
    if (channel)
      return "on#" + std::to_string(std::max(1, int((n * 100 + 127) / 255)));
    const int speeds[] = {0, 20, 50, 75, 100};
    unsigned best = 1;
    double distance = 101;
    for (unsigned i = 1; i < 5; i++) {
      double delta = n - speeds[i];
      if (delta < 0)
        delta = -delta;
      if (delta <= distance) {
        best = i;
        distance = delta;
      }
    }
    return "on#" + std::to_string(best);
  }

private:
  DynamicJsonDocument &config;
  HubService &service;
  HubBroker &broker;
  uint32_t clock = 0;
  struct Echo {
    uint64_t hash = 0;
    uint32_t at = 0;
    unsigned source = 0;
    bool used = false;
  };
  struct Pending {
    std::string id, base;
    uint32_t at = 0;
    bool used = false;
  };
  struct Device {
    std::string base;
    uint32_t at = 0;
    bool online = false, seen = false, freshAir = false;
  };
  struct Report {
    std::string body;
    uint32_t at = 0;
  };
  std::array<Echo, 16> echoes;
  std::array<Pending, 8> pending;
  std::array<Device, HubService::Devices + 1> devices;
  std::array<Report, HubService::Mappings> reported;
  unsigned echoSlot = 0;
  std::string base(JsonObjectConst d) const {
    return service.prefix() + std::string(d["model"] | "") + "/" +
           std::string(d["mac"] | "");
  }
  static uint64_t hash(const std::string &topic, const std::string &body) {
    uint64_t h = 14695981039346656037ULL;
    for (uint8_t c : topic) {
      h ^= c;
      h *= 1099511628211ULL;
    }
    h ^= 0;
    h *= 1099511628211ULL;
    for (uint8_t c : body) {
      h ^= c;
      h *= 1099511628211ULL;
    }
    return h;
  }
  void remember(const std::string &topic, const std::string &body,
                unsigned source) {
    auto &e = echoes[echoSlot++ % echoes.size()];
    e.used = true;
    e.hash = hash(topic, body);
    e.source = source;
    e.at = clock;
  }
  bool echo(const std::string &topic, const std::string &body,
            unsigned source) const {
    auto h = hash(topic, body);
    for (const auto &e : echoes)
      if (e.used && e.source == source && e.hash == h &&
          uint32_t(clock - e.at) < 5000)
        return true;
    return false;
  }
  void updateDevice(const std::string &topic, const std::string &body) {
    for (auto &p : pending)
      if (p.used && topic == p.base + "/result") {
        DynamicJsonDocument doc(1024);
        if (!hubWire.decode(doc, p.base, body) && doc["request_id"] == p.id) {
          p.used = false;
          std::string code = doc["code"] | "";
          status = code == "ok" || code == "accepted" ? "connected"
                                                      : "device_rejected";
        }
      }
    for (JsonObjectConst saved :
         config["hub"]["devices"].as<JsonArrayConst>()) {
      auto target = base(saved);
      if (topic != target + "/availability" && topic != target + "/state")
        continue;
      Device *slot = nullptr;
      for (auto &d : devices) {
        if (d.base == target) {
          slot = &d;
          break;
        }
        if (d.base.empty() && !slot)
          slot = &d;
      }
      if (!slot)
        return;
      slot->base = target;
      if (topic == target + "/availability") {
        slot->online = body == "online";
        if (!slot->online)
          slot->seen = false;
      } else {
        DynamicJsonDocument state(8192);
        if (body.size() > HubBroker::Packet || hubWire.decode(state, topic.substr(0,topic.size()-6), body) ||
            !state.is<JsonObject>())
          return;
        if (saved["model"] == "a1" &&
            (slot->freshAir || state.containsKey("fresh_air_on") ||
             state.containsKey("fresh_air_speed"))) {
          auto on = state["fresh_air_on"], speed = state["fresh_air_speed"];
          if (!on.is<bool>() || !speed.is<unsigned>() || speed.as<unsigned>() > 100 ||
              on.as<bool>() != (speed.as<unsigned>() > 0)) {
            slot->seen = false;
            return;
          }
          slot->freshAir = true;
        }
        slot->seen = true;
        slot->online = true;
        slot->at = clock;
      }
      return;
    }
  }
};
} // namespace iotcloud
