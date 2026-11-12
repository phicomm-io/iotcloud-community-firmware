/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "hub_device_bus.h"
#include <ArduinoJson.h>

namespace iotcloud {
// One local owner; cloud identities and credentials never authorize this
// service.
class HubService {
public:
  static constexpr size_t Devices = 12, Routes = 8, Mappings = 8,
                          ConfigCapacity = 16384;
  struct Reply {
    int status = 200;
    std::string body = "{}";
  };
  struct Request {
    std::string method, path, body, host;
    // The HTTP adapter captures and revalidates common ph_auth before dispatch.
    bool webAuthorized = false;
    Request(std::string m, std::string p, std::string b = "",
            bool authorized = false, std::string h = "")
        : method(std::move(m)), path(std::move(p)), body(std::move(b)),
          host(std::move(h)), webAuthorized(authorized) {}
  };
  std::function<bool(DynamicJsonDocument &)> store;
  std::function<std::string(size_t)> randomHex;
  std::function<void(bool)>
      changed; // true: namespace/identity changed; retire old connections.
  std::function<std::string()> bemfaStatus;
  std::function<void(JsonObject)> homeStatus;
  // Owner-authenticated, derived LAN inventory only; never serialize peer keys.
  std::function<void(JsonObject)> lanStatus;
  std::function<void(JsonObject)> deviceMetadata;
  // Synchronous borrow: asynchronous operations must copy input before return.
  std::function<const char *(const std::string &,JsonObjectConst,JsonObject)> lanOperation;
  std::function<bool()> homeReset;
  // Runtime identity, not an owner-editable registration or a stored slot.
  std::string localMac;
  explicit HubService(DynamicJsonDocument &configuration)
      : config(configuration) {}
  bool ready() const {
    const char *name = config["hub"]["username"] | "";
    return *name != 0;
  }
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
           s.find('\0') == std::string::npos && HubDeviceBus::utf8(s);
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
            model == "daikin" || model == "s30-v1b") &&
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
          !HubDeviceBus::topicValid(local) || !HubDeviceBus::topicValid(remote) ||
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
      if ((model != "tc1" && model != "a1" && model != "s30-v1b") ||
          channel > (model == "tc1" ? 5u : model == "s30-v1b" ? 0u : 1u))
        return false;
      std::string suffix = model == "tc1" || model == "s30-v1b" ? "001" : channel ? "002" : "003";
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
      return true; // Hub startup supplies the stable local namespace.
    JsonObjectConst h = value.as<JsonObjectConst>();
    const bool legacy = h["schema"] == 1;
    const char *allowed = legacy
        ? "|schema||username||salt||password_hash||mqtt_salt||mqtt_hash||mqtt_password_separate||devices||bridge||bemfa||homekit||instance|"
        : "|schema||username||devices||bridge||bemfa||homekit||instance|";
    if (!fields(h, allowed) || !h["schema"].is<unsigned>() ||
        (!legacy && h["schema"] != 2) || !nameValid(h["username"]) ||
        !h["devices"].is<JsonArrayConst>()) return false;
    if (legacy && (!hex(h["salt"],32) || !hex(h["password_hash"],64) ||
        !hex(h["mqtt_salt"],32) || !hex(h["mqtt_hash"],64) ||
        !h["mqtt_password_separate"].is<bool>())) return false;
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
  bool initializeIdentity() {
    if (!valid(config["hub"])) return false;
    if (config["hub"]["schema"] == 2) return true;
    const size_t capacity = std::min<size_t>(size_t(ConfigCapacity), config.memoryUsage()+1024);
    DynamicJsonDocument next(capacity);
    if (!next.capacity() || !next.set(config)) return false;
    auto hub = next["hub"].as<JsonObject>();
    if (hub.isNull()) {
      hub = next.createNestedObject("hub");
      hub["username"] = "admin";
      hub["instance"] = randomHex ? randomHex(16) : "";
      if (!hex(hub["instance"],32)) return false;
      hub.createNestedArray("devices");
    }
    hub["schema"] = 2;
    for (const auto *key : {"salt","password_hash","mqtt_salt","mqtt_hash","mqtt_password_separate"})
      hub.remove(key);
    return commit(next, false);
  }
  size_t workingMemory(const Request &request) const {
    return inputCapacity(request) + outputCapacity(request) + responseCapacity(request) +
           (needsCandidate(request) ? candidateCapacity(request) : 0) + 16384;
  }
  size_t largestAllocation(const Request &request) const {
    return std::max(needsCandidate(request) ? candidateCapacity(request) : size_t(2048),
                    std::max(responseCapacity(request),
                             std::max(inputCapacity(request), outputCapacity(request))));
  }
  Reply handle(const Request &request, uint32_t now) {
    const auto &path = request.path;
    const bool get = request.method == "GET";
    // Most requests contain only credentials or a single device. Reserving two
    // maximum-size documents here starves the ESP32 byte-addressable heap.
    DynamicJsonDocument input(inputCapacity(request)), output(outputCapacity(request));
    if ((inputCapacity(request) && !input.capacity()) || !output.capacity())
      return fail(503, "busy");
    if (!get && !(request.method == "DELETE" && request.body.empty()) &&
        (request.body.empty() || deserializeJson(input, request.body.c_str(), request.body.size()) ||
         !input.is<JsonObject>()))
      return fail(400, "invalid_params");
    if (path == "/api/register/status" && get) {
      output["enabled"] = false;
      output["hub"] = true;
      output["setup_required"] = false;
      output["auth"] = "webui";
      output["lan_protocol"] = 4;
      output["lan_codec"] = "msgpack";
      output["lan_discovery"] = "mdns";
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
    if (!request.webAuthorized) return fail(401, "unauthorized");
    if (path == "/api/auth/me" && get) {
      account(output);
      return json(output);
    }
    const std::string lanPairPrefix="/api/hub/lan/pair/";
    if (path == "/api/hub/lan/pair" || path == "/api/hub/lan/unpair" ||
        path.compare(0,lanPairPrefix.size(),lanPairPrefix)==0) {
      std::string op;
      if (path == "/api/hub/lan/pair") {
        if (request.method!="POST") return fail(405,"method_not_allowed");
        if (input.size()!=4 || !fields(input.as<JsonObjectConst>(),"|peer||ip||password||scopes|") ||
            !hex(input["peer"],12) || !string(input["ip"],15,7) ||
            !string(input["password"],32) || !input["scopes"].is<unsigned>() || input["scopes"]!=1)
          return fail(400,"invalid_params");
        const std::string ip=input["ip"].as<std::string>();
        unsigned parts=0,value=0,digits=0;
        for(size_t i=0;i<=ip.size();++i){
          if(i==ip.size() || ip[i]=='.'){
            if(!digits || value>255 || ++parts>4) return fail(400,"invalid_params");
            digits=value=0;
          }else if(ip[i]>='0'&&ip[i]<='9'){
            if(++digits>3) return fail(400,"invalid_params");
            value=value*10+unsigned(ip[i]-'0');
          }else return fail(400,"invalid_params");
        }
        if(parts!=4) return fail(400,"invalid_params");
        op="pair";
      }else if(path == "/api/hub/lan/unpair"){
        if(request.method!="POST") return fail(405,"method_not_allowed");
        if(input.size()!=1 || !fields(input.as<JsonObjectConst>(),"|peer|") || !hex(input["peer"],12))
          return fail(400,"invalid_params");
        op="unpair";
      }else{
        if(!get) return fail(405,"method_not_allowed");
        const std::string peer=path.substr(lanPairPrefix.size());
        // GET normally has no input pool; keep the generated request bounded.
        if(peer.size()!=12 || peer.find_first_not_of("0123456789abcdef")!=std::string::npos)
          return fail(400,"invalid_params");
        if(!lanOperation) return fail(503,"not_ready");
        DynamicJsonDocument statusInput(128);statusInput["peer"]=peer;
        return lanReply("status",statusInput.as<JsonObjectConst>(),output);
      }
      if(!lanOperation) return fail(503,"not_ready");
      return lanReply(op,input.as<JsonObjectConst>(),output);
    }
    if (path == "/api/hub/lan") {
      if (!get) return fail(405, "method_not_allowed");
      output["protocol"] = 4;
      output["codec"] = "msgpack";
      output["discovery"] = "mdns";
      output["ready"] = false;
      output.createNestedArray("devices");
      if (lanStatus) lanStatus(output.as<JsonObject>());
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
      DynamicJsonDocument next(candidateCapacity(request));
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
    if (path == "/api/devices") {
      if (get) {
        auto devices = output.to<JsonArray>();
        if (!localMac.empty()) {
          auto self = devices.createNestedObject();
          self["mac"] = localMac;
          self["model"] = "daikin";
          self["version"] = "v2";
          self["status"] = "active";
          self["username"] = user();
          self["self"] = true;
          if (deviceMetadata) deviceMetadata(self);
        }
        for (JsonObjectConst device : config["hub"]["devices"].as<JsonArrayConst>())
          if (localMac.empty() || device["mac"] != localMac)
            { auto row = devices.createNestedObject(); row.set(device);
              if (deviceMetadata) deviceMetadata(row); }
        return json(output);
      }
      if (request.method != "POST")
        return fail(405, "method_not_allowed");
      input["status"] = "active";
      input["username"] = user();
      if (!deviceValid(input.as<JsonObjectConst>()))
        return fail(400, "invalid_params");
      if (!localMac.empty() && input["mac"] == localMac)
        return fail(409, "conflict");
      for (JsonObjectConst d : config["hub"]["devices"].as<JsonArrayConst>())
        if (d["mac"] == input["mac"])
          return fail(409, "conflict");
      if (config["hub"]["devices"].size() >= Devices)
        return fail(409, "hub_capacity");
      DynamicJsonDocument next(candidateCapacity(request));
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
      if (!localMac.empty() && id == localMac)
        return fail(403, "hub_local_device");
      DynamicJsonDocument next(candidateCapacity(request));
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
      DynamicJsonDocument next(candidateCapacity(request));
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
          Request{"GET", path, "", request.webAuthorized, request.host}, now);
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
      DynamicJsonDocument next(candidateCapacity(request));
      next.set(config);
      if (request.method == "DELETE")
        next["hub"].remove("bemfa");
      else {
        if (!fields(input.as<JsonObjectConst>(),
                    "|enabled||api_key||mappings|"))
          return fail(400, "invalid_params");
        if (input["api_key"] == "")
          input["api_key"] =
              std::string(config["hub"]["bemfa"]["api_key"] | "");
        if (!bemfaValid(input.as<JsonObjectConst>(), config["hub"]["devices"]))
          return fail(400, "invalid_params");
        next["hub"]["bemfa"] = input.as<JsonObjectConst>();
      }
      if (!commit(next, false))
        return fail(503, "storage_error");
      return handle(
          Request{"GET", path, "", request.webAuthorized, request.host}, now);
    }
    return fail(404, "not_found");
  }
  static bool command(const std::string &topic) {
    return suffix(topic, "/command") || suffix(topic, "/set");
  }

private:
  Reply lanReply(const std::string &op,JsonObjectConst input,DynamicJsonDocument &output){
    const char *result=lanOperation(op,input,output.to<JsonObject>());
    const std::string code=result?result:"internal_error";
    int status=500;
    if(code=="ok") status=200;
    else if(code=="accepted") status=202;
    else if(code=="busy"||code=="storage_error"||code=="not_ready") status=503;
    else if(code=="invalid_params") status=400;
    else if(code=="auth_required"||code=="unauthorized"||code=="forbidden") status=403;
    else if(code=="migration_required") status=409;
    if(op=="status"&&code=="ok"){
      // HTTP 200 reports a successful read; keep the pairing task's own code.
      output["operation_code"]="ok";
      if(!output.containsKey("code"))output["code"]="ok";
    }else output["code"]=status==500?"internal_error":code;
    return json(output,status);
  }
  static bool needsCandidate(const Request &request) {
    return request.method != "GET" &&
           request.path != "/api/hub/lan/pair" &&
           request.path != "/api/hub/lan/unpair" &&
           request.path != "/api/hub/homekit/pairing";
  }
  static size_t inputCapacity(const Request &request) {
    return request.method == "GET" || request.body.empty()
               ? 0
               : std::min<size_t>(12288, request.body.size() * 2 + 512);
  }
  size_t candidateCapacity(const Request &request) const {
    // Copy the compact current pool plus the request's bounded input pool.
    // The margin covers namespace/device metadata and
    // new object slots. Small edits must remain possible with HAP running,
    // even when no contiguous maximum-size config buffer is available.
    return std::min<size_t>(size_t(ConfigCapacity),
        config.memoryUsage() + inputCapacity(request) + 1024);
  }
  JsonVariantConst responseValue(const Request &request) const {
    JsonVariantConst value;
    if (request.method == "GET") {
      if (request.path == "/api/devices") value = config["hub"]["devices"];
      else if (request.path == "/api/hub/bridge") value = config["hub"]["bridge"];
      else if (request.path == "/api/integrations/bemfa") value = config["hub"]["bemfa"];
    }
    return value;
  }
  size_t outputCapacity(const Request &request) const {
    if (request.method == "GET" && (request.path == "/api/hub/lan" || request.path == "/api/devices")) return 8192;
    return std::max<size_t>(2048, responseValue(request).memoryUsage() + 1024);
  }
  size_t responseCapacity(const Request &request) const {
    if (request.method == "GET" && (request.path == "/api/hub/lan" || request.path == "/api/devices")) return 8192;
    // Repeated JSON strings share pool storage, while escaped strings expand
    // on the wire. Budget the serialized bytes separately from the JSON pool.
    // The margin covers bridge MQTT metadata (127-byte host, 63-byte user,
    // escaped up to sixfold), status fields and the terminating byte.
    return std::max<size_t>(2048, measureJson(responseValue(request)) + 1536);
  }
  DynamicJsonDocument &config;
  static bool suffix(const std::string &value, const std::string &end) {
    return value.size() >= end.size() &&
           value.compare(value.size() - end.size(), end.size(), end) == 0;
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
  bool commit(DynamicJsonDocument &next, bool credentials) {
    if (next.overflowed() || !valid(next["hub"]) || !store || !store(next))
      return false;
    if (changed)
      changed(credentials);
    return true;
  }
  void account(DynamicJsonDocument &out) const {
    out["username"] = user();
    out["mqtt_username"] = user();
    out["is_admin"] = false;
    out["hub"] = true;
    out["auth"] = "webui";
    out["password_set"] = strlen(config["auth"]["password"] | "") > 0;
    out["auth_required"] = out["password_set"];
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
    reply.body.reserve(measureJson(doc));
    serializeJson(doc, reply.body);
    return reply;
  }
};
} // namespace iotcloud
