/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "hub_service.h"
#include <cmath>

namespace iotcloud {
// The protocol-neutral bridge model accepts only the local owner's registered
// devices. Controllers see measured state; a control succeeds only after a
// matching TMCP receipt and a subsequent state snapshot confirm it.
class HubHome {
public:
  static constexpr size_t Capacity = HubService::Devices + 1;
  enum Metric { Temperature, Humidity, PM25, PM10, CO2, TVOC, MetricCount };
  enum Control { Outlet, FanOn, FanSpeed, LightOn, LightBrightness };
  struct Value {
    double number = 0;
    uint32_t at = 0;
    bool valid = false;
    bool fresh(uint32_t now) const {
      return valid && uint32_t(now - at) < 20000;
    }
  };
  struct Device {
    std::string model, mac, base, name, query;
    uint32_t aid = 0, at = 0, sequence = 0, queriedAt = 0;
    bool seen = false, online = false, offline = false, controls = false;
    bool outlets[6] = {}, fan = false, light = false, freshAir = false;
    unsigned speed = 0, brightness = 0;
    std::array<Value, MetricCount> values;
    bool reachable(uint32_t now) const {
      return online && seen && uint32_t(now - at) < 30000;
    }
  };
  std::array<Device, Capacity> devices;
  size_t size = 0;
  std::function<std::string(size_t)> randomHex;
  explicit HubHome(HubBroker &broker) : broker(broker) {}
  // Reconfiguration discards old state and pending commands, preventing an
  // account/device change from inheriting a previous device's readings.
  bool configure(JsonArrayConst saved, const std::string &prefix,
                 const std::string &ownMac) {
    for (auto &d : devices)
      d = Device();
    size = 0;
    pending = Pending();
    if (!add("daikin", ownMac, prefix))
      return false;
    for (JsonObjectConst d : saved) {
      if (d["mac"] == ownMac)
        continue;
      if (!add(d["model"] | "", d["mac"] | "", prefix)) {
        size = 0;
        return false;
      }
    }
    return true;
  }
  void tick(uint32_t now) { clock = now; }
  void observe(const std::string &topic, const std::string &body) {
    for (size_t i = 0; i < size; i++) {
      auto &d = devices[i];
      if (topic == d.base + "/availability") {
        d.online = body == "online";
        d.offline = !d.online;
        if (!d.online) {
          d.seen = false;
          for (auto &v : d.values)
            v.valid = false;
        }
        return;
      }
      bool result = topic == d.base + "/result";
      if (topic != d.base + "/state" && !result)
        continue;
      if (body.size() > HubBroker::Packet)
        return;
      size_t capacity = std::min(size_t(8192), body.size() * 2 + 512);
      for (;;) {
        DynamicJsonDocument doc(capacity);
        if (!doc.capacity())
          return;
        auto error = deserializeJson(doc, body);
        if (error == DeserializationError::NoMemory && capacity < 8192) {
          // Dense JSON can need far more pool space than its wire length.
          // Release this pool before retrying, preserving the existing limit.
          capacity = std::min(size_t(8192), capacity + 1024);
          continue;
        }
        if (error || !doc.is<JsonObject>())
          return;
        if (result) {
          if (pending.used && pending.device == i &&
              doc["request_id"] == pending.id) {
            bool accepted = doc["code"] == "ok" || doc["code"] == "accepted";
            if (accepted && !pending.acked) {
              pending.acked = true;
              pending.sequence = d.sequence;
              query(d); // Confirm a snapshot taken after the command receipt.
            }
            pending.failed = !accepted;
          }
          if (d.query.empty() || doc["request_id"] != d.query ||
              doc["code"] != "ok" || !doc["data"].is<JsonObject>())
            return;
          d.query.clear();
          state(d, doc["data"]);
        } else
          state(d, doc.as<JsonObjectConst>());
        return;
      }
    }
  }
  // At most one small status query per tick; no retained data is treated as a
  // confirmation. Each result must match the current random request ID.
  void refresh() {
    for (size_t n = 0; n < size; n++) {
      auto &d = devices[queryCursor++ % size];
      if (uint32_t(clock - d.queriedAt) < 10000 ||
          (d.seen && uint32_t(clock - d.at) < 5000))
        continue;
      query(d);
      break;
    }
  }
  // A level and an optional power change share one TMCP mutation. Explicit
  // power-off wins over a nonzero level in the same HomeKit update.
  bool begin(size_t index, Control control, unsigned channel, double desired,
             bool powerUpdated = false, bool power = false) {
    if (pending.used || index >= size || !std::isfinite(desired))
      return false;
    if (control < Outlet || control > LightBrightness ||
        (powerUpdated && control != FanSpeed && control != LightBrightness))
      return false;
    const auto &d = devices[index];
    if (!d.reachable(clock) || !d.controls)
      return false;
    DynamicJsonDocument request(512);
    auto params = request.createNestedObject("params");
    if (control == Outlet) {
      if (d.model != "tc1" || channel > 5 || (desired != 0 && desired != 1))
        return false;
      request["action"] = "outlet.set";
      params["index"] = channel;
      params["on"] = desired != 0;
    } else {
      if (d.model != "a1" || channel != 0)
        return false;
      request["action"] = control == FanOn || control == FanSpeed
                              ? (d.freshAir ? "fresh_air.set" : "fan.set")
                              : "light.set";
      if (control == FanOn || control == LightOn) {
        if (desired != 0 && desired != 1)
          return false;
        params[control == FanOn ? "on" : "enabled"] = desired != 0;
      } else {
        if (desired < 0 || desired > 100 || std::floor(desired) != desired)
          return false;
        params[control == FanSpeed ? "on" : "enabled"] =
            desired > 0 && (!powerUpdated || power);
        if (control == FanSpeed) {
          params["mode"] = "manual";
          params["speed"] = unsigned(desired);
        } else
          params["brightness"] = unsigned(desired);
      }
    }
    pending = Pending();
    pending.id = identifier();
    if (pending.id.empty())
      return false;
    pending.used = true;
    pending.device = index;
    pending.control = control;
    pending.channel = channel;
    pending.desired = desired;
    pending.power = desired > 0 && (!powerUpdated || power);
    pending.sequence = d.sequence;
    request["request_id"] = pending.id;
    std::string body;
    serializeJson(request, body);
    if (!broker.publish(d.base + "/command", body, false,
                        HubBroker::Origin::Internal)) {
      pending = Pending();
      return false;
    }
    return true;
  }
  bool finished() const {
    return pending.used &&
           (pending.failed || (pending.acked && confirmed()));
  }
  bool finish() {
    bool ok = pending.used && !pending.failed && pending.acked && confirmed();
    pending = Pending();
    return ok;
  }

private:
  HubBroker &broker;
  uint32_t clock = 0;
  size_t queryCursor = 0;
  struct Pending {
    bool used = false, acked = false, failed = false, power = false;
    size_t device = 0;
    Control control = Outlet;
    unsigned channel = 0;
    double desired = 0;
    uint32_t sequence = 0;
    std::string id;
  } pending;
  bool add(const std::string &model, const std::string &mac,
           const std::string &prefix) {
    if (size >= Capacity || mac.size() != 12 ||
        (model != "tc1" && model != "a1" && model != "m1" && model != "daikin"))
      return false;
    uint32_t aid = 2166136261u;
    for (uint8_t c : model + "/" + mac)
      aid = (aid ^ c) * 16777619u;
    aid = (aid & 0x7fffffffu) + 2;
    for (size_t i = 0; i < size; i++)
      if (devices[i].aid == aid || devices[i].mac == mac)
        return false; // Never silently give two devices the same HAP identity.
    auto &d = devices[size++];
    d.aid = aid;
    d.model = model;
    d.mac = mac;
    d.base = prefix + model + "/" + mac;
    d.name = model + " " + mac.substr(6);
    return true;
  }
  std::string identifier() {
    auto id = randomHex ? randomHex(12) : "";
    return id.size() == 24 ? "hub_home_" + id : "";
  }
  void query(Device &d) {
    d.queriedAt = clock;
    d.query = identifier();
    if (!d.query.empty() &&
        !broker.publish(d.base + "/command",
                        "{\"action\":\"status.get\",\"request_id\":\"" +
                            d.query + "\"}",
                        false, HubBroker::Origin::Internal))
      d.query.clear();
  }
  void value(Device &d, Metric key, JsonVariantConst v, bool valid,
             double minimum, double maximum, uint32_t age = 0) {
    auto &out = d.values[key];
    out.valid = valid && v.is<double>() && std::isfinite(v.as<double>()) &&
                v.as<double>() >= minimum && v.as<double>() <= maximum &&
                age < 20000;
    if (out.valid) {
      out.number = v;
      out.at = clock - age;
    }
  }
  void state(Device &d, JsonObjectConst doc) {
    if (d.offline || (!doc["model"].isNull() && doc["model"] != d.model) ||
        (!doc["device_id"].isNull() && canonicalMac(doc["device_id"]) != d.mac))
      return;
    bool valid = false;
    if (d.model == "tc1") {
      auto outlets = doc["socket_states"].as<JsonArrayConst>();
      valid = outlets.size() == 6;
      for (JsonVariantConst v : outlets)
        valid &= v.is<unsigned>() && v.as<unsigned>() <= 1;
      if (valid)
        for (size_t i = 0; i < 6; i++)
          d.outlets[i] = outlets[i] == 1;
      d.controls = valid;
    } else if (d.model == "a1") {
      // New-air variants report the independent fan's applied output. Its
      // requested_on/speed may remain nonzero while shutdown/linkage gates it.
      bool fresh = d.freshAir || doc.containsKey("fresh_air_on") ||
                   doc.containsKey("fresh_air_speed");
      auto on = doc[fresh ? "fresh_air_on" : "fan_on"],
           speed = doc[fresh ? "fresh_air_speed" : "percentage"];
      valid = on.is<bool>() && speed.is<unsigned>() &&
              speed.as<unsigned>() <= 100 &&
              (!fresh || on.as<bool>() == (speed.as<unsigned>() > 0)) &&
              doc["led_visible"].is<bool>() && doc["brightness"].is<unsigned>() &&
              doc["brightness"].as<unsigned>() <= 255;
      if (valid) {
        d.freshAir = fresh;
        d.fan = on;
        d.speed = speed;
        d.light = doc["led_visible"];
        d.brightness = doc["brightness"];
      }
      d.controls = valid;
    } else if (d.model == "m1") {
      valid = doc["sensor_valid"].is<bool>();
      uint32_t age = doc["age_ms"].is<unsigned>()
                         ? doc["age_ms"].as<unsigned>()
                         : 20000;
      bool climate = doc["climate_valid"] == true;
      value(d, Temperature, doc["temperature"], climate, -100, 100, age);
      value(d, Humidity, doc["humidity"], climate, 0, 100, age);
      value(d, PM25, doc["pm25"], doc["pm25_valid"] == true, 0, 1000, age);
      auto co2 = doc["scd40"];
      value(d, CO2, co2["co2_ppm"], co2["valid"] == true, 1, 100000,
            co2["age_ms"].is<unsigned>() ? co2["age_ms"].as<unsigned>() : 20000);
    } else {
      valid = doc["sensors"].is<JsonObjectConst>();
      static const char *keys[] = {"temperature", "humidity", "pm25", "pm10",
                                   "co2", "tvoc"};
      static const char *units[] = {"°C", "%", "µg/m³", "µg/m³", "ppm", "ppb"};
      const double minimum[] = {-100, 0, 0, 0, 1, 0},
                   maximum[] = {100, 100, 1000, 1000, 100000, 60000};
      for (unsigned i = 0; i < MetricCount; i++) {
        auto sensor = doc["sensors"][keys[i]];
        value(d, Metric(i), sensor["value"],
              sensor["valid"] == true && sensor["unit"] == units[i], minimum[i],
              maximum[i], sensor["age_ms"].is<unsigned>()
                              ? sensor["age_ms"].as<unsigned>()
                              : 20000);
      }
    }
    if (!valid)
      return;
    if (HubService::string(doc["name"], 63, 1))
      d.name = doc["name"].as<std::string>();
    d.seen = d.online = true;
    d.at = clock;
    d.sequence++;
  }
  bool confirmed() const {
    const auto &d = devices[pending.device];
    if (d.sequence == pending.sequence || !d.reachable(clock))
      return false;
    switch (pending.control) {
    case Outlet: return d.outlets[pending.channel] == bool(pending.desired);
    case FanOn: return d.fan == bool(pending.desired);
    case FanSpeed:
      return !pending.power ? !d.fan : d.fan && d.speed == pending.desired;
    case LightOn: return d.light == bool(pending.desired);
    case LightBrightness:
      return !pending.power ? !d.light
          : d.light && d.brightness == unsigned((pending.desired * 255 + 50) / 100);
    }
    return false;
  }
  static std::string canonicalMac(JsonVariantConst value) {
    if (!value.is<const char *>())
      return {};
    auto id = value.as<std::string>();
    for (auto &c : id)
      if (c >= 'A' && c <= 'F')
        c += 'a' - 'A';
    return id;
  }
};
} // namespace iotcloud
