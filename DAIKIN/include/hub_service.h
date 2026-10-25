/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "hub_broker.h"
#include <ArduinoJson.h>

namespace iotcloud {
// One local owner; cloud identities and credentials never authorize this
// service.
class HubService {
public:
  static constexpr size_t Devices = 12, Routes = 8, Mappings = 8, Sessions = 4;
  struct Reply {
    int status = 200;
    std::string body = "{}", token;
  };
  struct Request {
    std::string method, path, body, token, host;
    bool setupAuthorized = false;
    Request(std::string m, std::string p, std::string b = "",
            std::string t = "", std::string h = "", bool setup = false)
        : method(std::move(m)), path(std::move(p)), body(std::move(b)),
          token(std::move(t)), host(std::move(h)), setupAuthorized(setup) {}
  };
  std::function<bool(DynamicJsonDocument &)> store;
  std::function<std::string(size_t)> randomHex;
  std::function<std::string(const std::string &, const std::string &)> derive;
  std::function<void(bool)>
      changed; // true: credentials changed, drop sessions/connections.
  std::function<std::string()> bemfaStatus;
  std::function<void(JsonObject)> homeStatus;
  std::function<bool()> homeReset;
  explicit HubService(DynamicJsonDocument &configuration)
      : config(configuration) {}
  bool ready() const { return !user().empty(); }
  std::string user() const { return config["hub"]["username"] | ""; }
  static std::string userKey(const std::string &name) {
    std::string key = "u_";
    for (uint8_t c : name) {
      key += "0123456789abcdef"[c >> 4];
      key += "0123456789abcdef"[c & 15];
    }
    return key;
  }
  std::string prefix() const { return "phicomm/v2/" + userKey(user()) + "/"; }
  bool allowed(const std::string &topic, bool) const {
    return ready() && (topic.compare(0, 11, "phicomm/v2/") ||
                       topic.compare(0, prefix().size(), prefix()) == 0);
  }
  bool authenticate(const std::string &username, const std::string &password,
                    uint32_t now) {
    if (!ready() || username != user() || password.size() > 128 ||
        throttled(now))
      return false;
    bool ok = verify(password, "mqtt_salt", "mqtt_hash");
    record(ok, now);
    return ok;
  }
  static bool fields(JsonObjectConst object, const char *names) {
    if (object.isNull())
      return false;
    for (JsonPairConst p : object) {
      std::string name = "|" + std::string(p.key().c_str()) + "|";
      if (std::string(names).find(name) == std::string::npos)
        return false;
    }
    return true;
  }
  static bool string(JsonVariantConst value, size_t max, size_t min = 0) {
    if (!value.is<const char *>())
      return false;
    auto s = value.as<std::string>();
    return s.size() >= min && s.size() <= max &&
           s.find('\0') == std::string::npos && HubBroker::utf8(s);
  }
  static bool hex(JsonVariantConst value, size_t length) {
    if (!string(value, length, length))
      return false;
    for (char c : value.as<std::string>())
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return false;
    return true;
  }
  static bool nameValid(JsonVariantConst value, size_t min = 3,
                        size_t max = 32) {
    if (!string(value, max, min))
      return false;
    for (char c : value.as<std::string>())
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_'))
        return false;
    return true;
  }
  static bool deviceValid(JsonObjectConst d) {
    std::string model = d["model"] | "";
    return fields(d, "|mac||model||version||status||username|") &&
           hex(d["mac"], 12) && d["mac"] != "000000000000" &&
           d["mac"] != "ffffffffffff" &&
           (model == "tc1" || model == "m1" || model == "a1" ||
            model == "daikin") &&
           d["version"] == "v2" && d["status"] == "active" &&
           nameValid(d["username"]);
  }
  static bool bridgeValid(JsonObjectConst bridge, const std::string &owner) {
    if (bridge.isNull())
      return true;
    if (!fields(bridge, "|enabled||mappings|") ||
        !bridge["enabled"].is<bool>() ||
        !bridge["mappings"].is<JsonArrayConst>())
      return false;
    auto routes = bridge["mappings"].as<JsonArrayConst>();
    if (routes.size() > Routes)
      return false;
    std::vector<std::string> seen;
    auto prefix = "phicomm/v2/" + userKey(owner) + "/";
    for (JsonObjectConst r : routes) {
      std::string direction = r["direction"] | "",
                  local = r["local_topic"] | "",
                  remote = r["remote_topic"] | "";
      if (!fields(r, "|direction||local_topic||remote_topic||retain||allow_"
                     "retained|") ||
          (direction != "in" && direction != "out") ||
          !HubBroker::topicValid(local) || !HubBroker::topicValid(remote) ||
          !r["retain"].is<bool>() || !r["allow_retained"].is<bool>() ||
          (!local.compare(0, 11, "phicomm/v2/") &&
           local.compare(0, prefix.size(), prefix)) ||
          (r["retain"].as<bool>() && (command(local) || command(remote))) ||
          (r["allow_retained"].as<bool>() &&
           (command(local) || command(remote))))
        return false;
      auto key = direction + "\n" + (direction == "in" ? remote : local);
      if (std::find(seen.begin(), seen.end(), key) != seen.end())
        return false;
      seen.push_back(key);
    }
    return !bridge["enabled"].as<bool>() || !routes.isNull();
  }
  static bool bemfaValid(JsonObjectConst b, JsonArrayConst devices) {
    if (b.isNull())
      return true;
    if (!fields(b, "|enabled||api_key||mappings|") ||
        !b["enabled"].is<bool>() || !b["mappings"].is<JsonArrayConst>())
      return false;
    std::string key = b["api_key"] | "";
    if (!key.empty() && !keyValid(b["api_key"]))
      return false;
    auto maps = b["mappings"].as<JsonArrayConst>();
    if (maps.size() > Mappings ||
        (b["enabled"] == true && (key.empty() || maps.size() == 0)))
      return false;
    std::vector<std::string> seenTopics, seenChannels;
    for (JsonObjectConst m : maps) {
      if (!fields(m, "|device||channel||topic|") || !deviceValid(m["device"]) ||
          !m["channel"].is<unsigned>() || !string(m["topic"], 64, 4))
        return false;
      auto d = m["device"].as<JsonObjectConst>();
      std::string model = d["model"] | "", topic = m["topic"] | "";
      unsigned channel = m["channel"];
      if ((model != "tc1" && model != "a1") ||
          channel > (model == "tc1" ? 5u : 1u))
        return false;
      std::string suffix = model == "tc1" ? "001" : channel ? "002" : "003";
      if (topic.compare(topic.size() - 3, 3, suffix))
        return false;
      for (char c : topic)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
          return false;
      bool found = false;
      for (JsonObjectConst saved : devices)
        if (equalDevice(d, saved))
          found = true;
      auto id = model + "/" + std::string(d["mac"] | "") + "/" +
                std::to_string(channel);
      if (!found ||
          std::find(seenTopics.begin(), seenTopics.end(), topic) !=
              seenTopics.end() ||
          std::find(seenChannels.begin(), seenChannels.end(), id) !=
              seenChannels.end())
        return false;
      seenTopics.push_back(topic);
      seenChannels.push_back(id);
    }
    return true;
  }
  static bool valid(JsonVariantConst value) {
    if (value.isNull())
      return true; // Upgrading a normal device starts with no local account.
    JsonObjectConst h = value.as<JsonObjectConst>();
    if (!fields(h, "|schema||username||salt||password_hash||mqtt_salt||mqtt_"
                   "hash||mqtt_password_separate||devices||bridge||bemfa||homekit||instance|") ||
        !h["schema"].is<unsigned>() || h["schema"] != 1 ||
        !nameValid(h["username"]) || !hex(h["salt"], 32) ||
        !hex(h["password_hash"], 64) || !hex(h["mqtt_salt"], 32) ||
        !hex(h["mqtt_hash"], 64) || !h["mqtt_password_separate"].is<bool>() ||
        !h["devices"].is<JsonArrayConst>())
      return false;
    auto devices = h["devices"].as<JsonArrayConst>();
    if (devices.size() > Devices)
      return false;
    std::vector<std::string> ids;
    for (JsonObjectConst d : devices) {
      std::string id = d["mac"] | "";
      if (!deviceValid(d) || d["username"] != h["username"] ||
          std::find(ids.begin(), ids.end(), id) != ids.end())
        return false;
      ids.push_back(id);
    }
    auto home = h["homekit"].as<JsonObjectConst>();
    if ((!h["instance"].isNull() && !hex(h["instance"], 32)) ||
        (!h["homekit"].isNull() &&
         (!fields(home, "|enabled|") || !home["enabled"].is<bool>())))
      return false;
    return bridgeValid(h["bridge"], h["username"] | "") &&
           bemfaValid(h["bemfa"], devices);
  }
  Reply handle(const Request &request, uint32_t now) {
    const auto &path = request.path;
    const bool get = request.method == "GET";
    DynamicJsonDocument input(12288), output(12288);
    if (!get && !(request.method == "DELETE" && request.body.empty()) &&
        (request.body.empty() || deserializeJson(input, request.body) ||
         !input.is<JsonObject>()))
      return fail(400, "invalid_params");
    if (path == "/api/register/status" && get) {
      output["enabled"] = false;
      output["hub"] = true;
      output["setup_required"] = !ready();
      output["mqtt_uri"] = "mqtt://" + request.host + ":1883";
      output["mqtt_ws_uri"] = "ws://" + request.host + ":9001/mqtt";
      return json(output);
    }
    if (path == "/api/auth/github/status" && get) {
      output["enabled"] = false;
      return json(output);
    }
    if (path == "/api/banner" && get) {
      output["enabled"] = false;
      return json(output);
    }
    if (path == "/api/hub/setup" && request.method == "POST") {
      if (ready())
        return fail(409, "hub_already_configured");
      if (!request.setupAuthorized)
        return fail(403, "set_web_password_first");
      if (!fields(input.as<JsonObjectConst>(), "|username||password|") ||
          !nameValid(input["username"]) || !string(input["password"], 63, 8))
        return fail(400, "invalid_params");
      DynamicJsonDocument next(config.capacity());
      next.set(config);
      auto h = next.createNestedObject("hub");
      h["schema"] = 1;
      h["username"] = input["username"];
      h["instance"] = randomHex ? randomHex(16) : "";
      h["mqtt_password_separate"] = false;
      h.createNestedArray("devices");
      if (!password(h, input["password"].as<std::string>(), false) ||
          !password(h, input["password"].as<std::string>(), true))
        return fail(503, "storage_error");
      if (!commit(next, true))
        return fail(503, "storage_error");
      output["code"] = "accepted";
      return json(output, 201);
    }
    if (path == "/api/auth/login" && request.method == "POST") {
      if (throttled(now))
        return fail(429, "hub_rate_limited");
      bool ok =
          ready() && input["username"] == user() &&
          string(input["password"], 128) &&
          verify(input["password"].as<std::string>(), "salt", "password_hash");
      record(ok, now);
      if (!ok)
        return fail(401, "unauthorized");
      auto token = randomHex ? randomHex(32) : "";
      if (token.size() != 64)
        return fail(503, "internal_error");
      size_t slot = sessionSlot++ % Sessions;
      for (size_t i = 0; i < Sessions; i++)
        if (!session[i].used) {
          slot = i;
          break;
        }
      session[slot].used = true;
      session[slot].token = token;
      session[slot].at = now;
      account(output);
      auto reply = json(output);
      reply.token = token;
      return reply;
    }
    if (!authorized(request.token, now))
      return fail(401, "unauthorized");
    if (path == "/api/auth/me" && get) {
      account(output);
      return json(output);
    }
    if (path == "/api/auth/logout" && request.method == "POST") {
      for (auto &s : session)
        if (constant(s.token, request.token))
          s = Session();
      return json(output);
    }
    if (path == "/api/hub/homekit") {
      if (get) {
        output["enabled"] = config["hub"]["homekit"]["enabled"] | false;
        output["protocol"] = "hap";
        if (homeStatus)
          homeStatus(output.as<JsonObject>());
        return json(output);
      }
      if (request.method != "PUT" ||
          !fields(input.as<JsonObjectConst>(), "|enabled|") ||
          !input["enabled"].is<bool>())
        return fail(400, "invalid_params");
      DynamicJsonDocument next(config.capacity());
      next.set(config);
      next["hub"]["homekit"] = input.as<JsonObjectConst>();
      if(next["hub"]["instance"].isNull()){
        auto id=randomHex?randomHex(16):"";
        if(id.size()!=32 || id.find_first_not_of("0123456789abcdef")!=std::string::npos)return fail(503,"storage_error");
        next["hub"]["instance"]=id;
      }
      if (!commit(next, false))
        return fail(503, "storage_error");
      output["code"] = "accepted";
      output["restart_required"] = true;
      return json(output, 202);
    }
    if (path == "/api/hub/homekit/pairing") {
      if (request.method != "DELETE" ||
          !fields(input.as<JsonObjectConst>(), "|confirm|") ||
          !input["confirm"].is<bool>() || input["confirm"] != true)
        return fail(400, "invalid_params");
      if (!homeReset || !homeReset())
        return fail(503, "storage_error");
      output["code"] = "accepted";
      output["restart_required"] = true;
      return json(output, 202);
    }
    if ((path == "/api/auth/password" || path == "/api/auth/mqtt-password") &&
        request.method == "POST") {
      if (!string(input["current_password"], 128) ||
          !string(input["password"], 63, 8))
        return fail(400, "invalid_params");
      if (throttled(now))
        return fail(429, "hub_rate_limited");
      bool ok = verify(input["current_password"].as<std::string>(), "salt",
                       "password_hash");
      record(ok, now);
      if (!ok)
        return fail(403, "unauthorized");
      bool mqtt = path == "/api/auth/mqtt-password";
      DynamicJsonDocument next(config.capacity());
      next.set(config);
      auto h = next["hub"].as<JsonObject>();
      if (!password(h, input["password"].as<std::string>(), mqtt))
        return fail(503, "storage_error");
      if (mqtt)
        h["mqtt_password_separate"] = true;
      else if (!h["mqtt_password_separate"].as<bool>() &&
               !password(h, input["password"].as<std::string>(), true))
        return fail(503, "storage_error");
      if (!commit(next, true, !mqtt))
        return fail(503, "storage_error");
      return json(output);
    }
    if (path == "/api/devices") {
      if (get) {
        output.set(config["hub"]["devices"]);
        return json(output);
      }
      if (request.method != "POST")
        return fail(405, "method_not_allowed");
      input["status"] = "active";
      input["username"] = user();
      if (!deviceValid(input.as<JsonObjectConst>()))
        return fail(400, "invalid_params");
      for (JsonObjectConst d : config["hub"]["devices"].as<JsonArrayConst>())
        if (d["mac"] == input["mac"])
          return fail(409, "conflict");
      if (config["hub"]["devices"].size() >= Devices)
        return fail(409, "hub_capacity");
      DynamicJsonDocument next(config.capacity());
      next.set(config);
      next["hub"]["devices"].as<JsonArray>().add(input.as<JsonObjectConst>());
      if (!commit(next, false))
        return fail(503, "storage_error");
      return json(input, 201);
    }
    if (path.compare(0, 13, "/api/devices/") == 0 &&
        request.method == "DELETE") {
      std::string id = path.substr(13);
      if (id.size() != 12)
        return fail(400, "invalid_params");
      DynamicJsonDocument next(config.capacity());
      next.set(config);
      auto devices = next["hub"]["devices"].as<JsonArray>();
      bool found = false;
      for (size_t i = 0; i < devices.size(); i++)
        if (devices[i]["mac"] == id) {
          devices.remove(i);
          found = true;
          break;
        }
      if (!found)
        return fail(404, "not_found");
      auto maps = next["hub"]["bemfa"]["mappings"].as<JsonArray>();
      for (size_t i = maps.size(); i > 0; i--)
        if (maps[i - 1]["device"]["mac"] == id)
          maps.remove(i - 1);
      if (maps.size() == 0 && !next["hub"]["bemfa"].isNull())
        next["hub"]["bemfa"]["enabled"] = false;
      if (!commit(next, false))
        return fail(503, "storage_error");
      return json(output);
    }
    if (path == "/api/hub/bridge") {
      if (get) {
        output.set(config["hub"]["bridge"]);
        if (!output.is<JsonObject>())
          output.to<JsonObject>();
        if (!output.containsKey("mappings"))
          output.createNestedArray("mappings");
        output["enabled"] = config["hub"]["bridge"]["enabled"] | false;
        auto upstream = output.createNestedObject("mqtt");
        upstream["host"] = config["mqtt"]["host"] | "";
        upstream["port"] = config["mqtt"]["port"] | 1883;
        upstream["user"] = config["mqtt"]["user"] | "";
        upstream["password_set"] = strlen(config["mqtt"]["password"] | "") > 0;
        return json(output);
      }
      if (request.method != "PUT" ||
          !fields(input.as<JsonObjectConst>(), "|enabled||mappings||mqtt|"))
        return fail(400, "invalid_params");
      DynamicJsonDocument next(config.capacity());
      next.set(config);
      auto upstream = input["mqtt"].as<JsonObjectConst>();
      if (!upstream.isNull()) {
        if (!fields(upstream, "|host||port||user||password|") ||
            !string(upstream["host"], 127) || !string(upstream["user"], 63) ||
            !upstream["port"].is<unsigned>() ||
            upstream["port"].as<unsigned>() < 1 ||
            upstream["port"].as<unsigned>() > 65535 ||
            (upstream.containsKey("password") &&
             !string(upstream["password"], 128)))
          return fail(400, "invalid_params");
        for (JsonPairConst p : upstream)
          next["mqtt"][p.key()] = p.value();
      }
      input.remove("mqtt");
      next["hub"]["bridge"] = input.as<JsonObjectConst>();
      if (!bridgeValid(next["hub"]["bridge"], user()))
        return fail(400, "invalid_params");
      if (!commit(next, false))
        return fail(503, "storage_error");
      return handle(
          Request{"GET", path, "", request.token, request.host, false}, now);
    }
    if (path == "/api/integrations/bemfa") {
      if (get) {
        output.set(config["hub"]["bemfa"]);
        if (!output.is<JsonObject>())
          output.to<JsonObject>();
        output.remove("api_key");
        output["enabled"] = config["hub"]["bemfa"]["enabled"] | false;
        output["key_set"] = strlen(config["hub"]["bemfa"]["api_key"] | "") > 0;
        if (!output.containsKey("mappings"))
          output.createNestedArray("mappings");
        output["max_mappings"] = unsigned(Mappings);
        output["status"] = bemfaStatus ? bemfaStatus() : "disabled";
        return json(output);
      }
      if (request.method != "PUT" && request.method != "DELETE")
        return fail(405, "method_not_allowed");
      DynamicJsonDocument next(config.capacity());
      next.set(config);
      if (request.method == "DELETE")
        next["hub"].remove("bemfa");
      else {
        if (!fields(input.as<JsonObjectConst>(),
                    "|enabled||api_key||mappings|"))
          return fail(400, "invalid_params");
        if (input["api_key"] == "")
          input["api_key"] = config["hub"]["bemfa"]["api_key"] | "";
        if (!bemfaValid(input.as<JsonObjectConst>(), config["hub"]["devices"]))
          return fail(400, "invalid_params");
        next["hub"]["bemfa"] = input.as<JsonObjectConst>();
      }
      if (!commit(next, false))
        return fail(503, "storage_error");
      return handle(
          Request{"GET", path, "", request.token, request.host, false}, now);
    }
    return fail(404, "not_found");
  }
  void clearSessions() {
    for (auto &s : session)
      s = Session();
  }
  static bool command(const std::string &topic) {
    return suffix(topic, "/command") || suffix(topic, "/set");
  }

private:
  DynamicJsonDocument &config;
  struct Session {
    bool used = false;
    std::string token;
    uint32_t at = 0;
  };
  std::array<Session, Sessions> session;
  unsigned sessionSlot = 0, failures = 0;
  uint32_t failureAt = 0;
  static bool suffix(const std::string &value, const std::string &end) {
    return value.size() >= end.size() &&
           value.compare(value.size() - end.size(), end.size(), end) == 0;
  }
  static bool constant(const std::string &a, const std::string &b) {
    size_t diff = a.size() ^ b.size();
    for (size_t i = 0; i < a.size(); i++)
      diff |= uint8_t(a[i]) ^ (i < b.size() ? uint8_t(b[i]) : 0);
    return !diff;
  }
  bool throttled(uint32_t now) const {
    return failures >= 5 && uint32_t(now - failureAt) < 30000;
  }
  void record(bool ok, uint32_t now) {
    if (ok || uint32_t(now - failureAt) >= 30000)
      failures = 0;
    if (!ok) {
      failures++;
      failureAt = now;
    }
  }
  bool verify(const std::string &pass, const char *salt,
              const char *hash) const {
    return derive && constant(derive(pass, config["hub"][salt] | ""),
                              config["hub"][hash] | "");
  }
  bool password(JsonObject hub, const std::string &pass, bool mqtt) {
    auto salt = randomHex ? randomHex(16) : "";
    auto hash = derive ? derive(pass, salt) : "";
    if (salt.size() != 32 || hash.size() != 64)
      return false;
    hub[mqtt ? "mqtt_salt" : "salt"] = salt;
    hub[mqtt ? "mqtt_hash" : "password_hash"] = hash;
    return true;
  }
  bool authorized(const std::string &token, uint32_t now) {
    if (token.size() != 64)
      return false;
    for (auto &s : session)
      if (s.used) {
        if (uint32_t(now - s.at) > 43200000)
          s = Session();
        else if (constant(s.token, token))
          return true;
      }
    return false;
  }
  static bool keyValid(JsonVariantConst key) {
    if (!string(key, 128, 16))
      return false;
    for (char c : key.as<std::string>())
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-'))
        return false;
    return true;
  }
  static bool equalDevice(JsonObjectConst a, JsonObjectConst b) {
    return a["mac"] == b["mac"] && a["model"] == b["model"] &&
           a["username"] == b["username"] && a["version"] == b["version"] &&
           b["status"] == "active";
  }
  bool commit(DynamicJsonDocument &next, bool credentials,
              bool invalidateSessions = true) {
    if (next.overflowed() || !valid(next["hub"]) || !store || !store(next))
      return false;
    if (credentials && invalidateSessions)
      clearSessions();
    if (changed)
      changed(credentials);
    return true;
  }
  void account(DynamicJsonDocument &out) const {
    out["username"] = user();
    out["mqtt_username"] = user();
    out["is_admin"] = false;
    out["hub"] = true;
    out["mqtt_password_separate"] = config["hub"]["mqtt_password_separate"];
    out.createNestedArray("aliases");
  }
  static Reply fail(int status, const char *code) {
    Reply reply;
    reply.status = status;
    reply.body = "{\"code\":\"" + std::string(code) + "\"}";
    return reply;
  }
  static Reply json(const DynamicJsonDocument &doc, int status = 200) {
    if (doc.overflowed())
      return fail(503, "internal_error");
    Reply reply;
    reply.status = status;
    reply.body.clear();
    serializeJson(doc, reply.body);
    return reply;
  }
};
} // namespace iotcloud
