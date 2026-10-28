#include "hub_relay.h"
#include <cassert>
#include <cstdio>
#include <tuple>
int main() {
  DynamicJsonDocument config(16384);
  assert(!deserializeJson(
      config,
      R"({"hub":{"username":"local","devices":[{"mac":"aabbccddeeff","model":"tc1","version":"v2","status":"active","username":"local"}],"bridge":{"enabled":true,"mappings":[{"direction":"out","local_topic":"home/local","remote_topic":"home/remote","retain":true,"allow_retained":false},{"direction":"in","local_topic":"home/in","remote_topic":"home/remote","retain":true,"allow_retained":false}]},"bemfa":{"enabled":true,"api_key":"0123456789abcdef","mappings":[{"device":{"mac":"aabbccddeeff","model":"tc1","version":"v2","status":"active","username":"local"},"channel":2,"topic":"socket001"}]}}})"));
  iotcloud::HubService service(config);
  iotcloud::HubDeviceBus bus;
  iotcloud::HubRelay relay(config, service, bus);
  using Event = std::tuple<std::string, std::string, bool>;
  std::vector<Event> remote, bemfa, local;
  bus.dispatch = [&](const std::string &t, const std::string &b, bool retain,
                     iotcloud::HubDeviceBus::Origin o) {
    local.emplace_back(t, b, retain);
    relay.local(t, b, retain, o);
    return true;
  };
  relay.upstream = [&](const std::string &t, const std::string &b, bool r) {
    remote.emplace_back(t, b, r);
    return true;
  };
  relay.bemfa = [&](const std::string &t, const std::string &b, bool r) {
    bemfa.emplace_back(t, b, r);
    return true;
  };
  relay.randomHex = [](size_t n) { return std::string(n * 2, 'a'); };
  relay.tick(100);
  bus.publish("home/local", "hello", true,
                 iotcloud::HubDeviceBus::Origin::Local);
  assert(remote.size() == 1 && std::get<0>(remote[0]) == "home/remote" &&
         std::get<2>(remote[0]));
  auto before = local.size();
  relay.incoming("home/remote", "hello", false);
  assert(local.size() == before);
  relay.incoming("home/remote", "different", true);
  assert(local.size() == before);
  relay.incoming("home/remote", "different", false);
  assert(local.size() == before + 1 && std::get<0>(local.back()) == "home/in" &&
         remote.size() == 1);
  relay.tick(5200);
  relay.incoming("home/remote", "hello", false);
  assert(local.size() == before + 2);
  assert(!relay.incomingBemfa("socket001", "on", false) &&
         relay.status == "device_offline");
  std::string base = service.prefix() + "tc1/aabbccddeeff";
  bus.publish(base + "/availability", "online", true,
                 iotcloud::HubDeviceBus::Origin::Local);
  bus.publish(base + "/state", R"({"socket_states":[0,0,1,0,0,0]})", true,
                 iotcloud::HubDeviceBus::Origin::Local);
  assert(bemfa.size() == 1 && std::get<1>(bemfa.back()) == "on");
  assert(!relay.incomingBemfa("socket001", "on", false)); // Own state echo.
  assert(
      !relay.incomingBemfa("socket001", "off", true)); // No retained commands.
  assert(!relay.incomingBemfa("socket001", "on#1", false));
  assert(relay.incomingBemfa("socket001", "off", false));
  DynamicJsonDocument command(1024);
  assert(!deserializeJson(command, std::get<1>(local.back())));
  assert(std::get<0>(local.back()) == base + "/command" &&
         command["action"] == "outlet.set" && command["params"]["index"] == 2 &&
         command["params"]["on"] == false);
  assert(remote.size() ==
         1); // Bemfa commands do not re-enter the upstream bridge.
  DynamicJsonDocument reply(256);
  reply["request_id"] = command["request_id"];
  reply["code"] = "accepted";
  std::string body;
  serializeJson(reply, body);
  bus.publish(base + "/result", body, false,
                 iotcloud::HubDeviceBus::Origin::Local);
  assert(relay.status == "connected");
  relay.tick(11000);
  assert(relay.incomingBemfa("socket001", "off", false));
  relay.tick(21001);
  assert(relay.status == "device_timeout");
  relay.tick(26001);
  assert(!relay.incomingBemfa("socket001", "off", false) &&
         relay.status == "device_offline");
  // Upstream routes are transport input, never local device evidence.
  for (const char *suffix : {"state", "availability", "result"}) {
    auto route = config["hub"]["bridge"]["mappings"].as<JsonArray>().createNestedObject();
    route["direction"] = "in";
    route["local_topic"] = base + "/" + suffix;
    route["remote_topic"] = std::string("injected/") + suffix;
    route["retain"] = true;
    route["allow_retained"] = true;
  }
  bus.publish(base + "/availability", "offline", true,
                 iotcloud::HubDeviceBus::Origin::Local);
  relay.tick(100000);
  for (bool retained : {false, true}) {
    auto deliveries = local.size();
    relay.incoming("injected/availability", "online", retained);
    relay.incoming("injected/state", R"({"socket_states":[0,0,1,0,0,0]})", retained);
    assert(local.size() == deliveries + 2); // The routes themselves are valid.
    assert(!relay.incomingBemfa("socket001", "off", false) &&
           relay.status == "device_offline");
  }
  bus.publish(base + "/state", R"({"socket_states":[0,0,1,0,0,0]})", true,
                 iotcloud::HubDeviceBus::Origin::Internal);
  assert(relay.incomingBemfa("socket001", "off", false));
  assert(!deserializeJson(command, std::get<1>(local.back())));
  reply["request_id"] = command["request_id"];
  reply["code"] = "accepted";
  body.clear();
  serializeJson(reply, body);
  relay.status = "awaiting_local_receipt";
  for (bool retained : {false, true}) {
    relay.incoming("injected/result", body, retained);
    assert(relay.status == "awaiting_local_receipt");
  }
  relay.tick(110001);
  assert(relay.status == "device_timeout");
  assert(relay.incomingBemfa("socket001", "off", false));
  bus.publish(base + "/result", body, false,
                 iotcloud::HubDeviceBus::Origin::Internal);
  assert(relay.status == "connected");
  DynamicJsonDocument mapping(256), state(1024), out(512);
  deserializeJson(mapping, R"({"device":{"model":"a1"},"channel":0})");
  assert(
      iotcloud::HubRelay::command(mapping.as<JsonObjectConst>(), "on#4", out) &&
      out["params"]["speed"] == 100);
  out.clear();
  assert(!iotcloud::HubRelay::command(mapping.as<JsonObjectConst>(), "on#04",
                                      out));
  deserializeJson(state, R"({"fan_on":true,"percentage":50})");
  assert(iotcloud::HubRelay::report(mapping.as<JsonObjectConst>(),
                                    state.as<JsonObjectConst>()) == "on#2");
  mapping["channel"] = 1;
  out.clear();
  assert(iotcloud::HubRelay::command(mapping.as<JsonObjectConst>(), "on#50",
                                     out) &&
         out["params"]["brightness"] == 50);
  deserializeJson(state, R"({"led_visible":true,"brightness":128})");
  assert(iotcloud::HubRelay::report(mapping.as<JsonObjectConst>(),
                                    state.as<JsonObjectConst>()) == "on#50");
  mapping["channel"] = 0;
  out.clear();
  assert(iotcloud::HubRelay::command(mapping.as<JsonObjectConst>(), "on#2", out,
                                    true) &&
         out["action"] == "fresh_air.set" && out["params"]["speed"] == 50);
  deserializeJson(state,
      R"({"fan_on":true,"percentage":100,"fresh_air_on":false,"fresh_air_speed":0,"fresh_air_requested_on":true,"fresh_air_requested_speed":50})");
  assert(iotcloud::HubRelay::report(mapping.as<JsonObjectConst>(),
                                    state.as<JsonObjectConst>()) == "off");
  deserializeJson(state,
      R"({"fan_on":false,"percentage":0,"fresh_air_on":true,"fresh_air_speed":75})");
  assert(iotcloud::HubRelay::report(mapping.as<JsonObjectConst>(),
                                    state.as<JsonObjectConst>()) == "on#3");
  // The actual relay route learns the variant from its latest applied state.
  config["hub"]["devices"][0]["model"] = "a1";
  config["hub"]["bemfa"]["mappings"][0]["device"]["model"] = "a1";
  config["hub"]["bemfa"]["mappings"][0]["channel"] = 0;
  config["hub"]["bemfa"]["mappings"][0]["topic"] = "fan003";
  relay.reset();
  const std::string fresh = service.prefix() + "a1/aabbccddeeff";
  bus.publish(fresh + "/state",
      R"({"fan_on":true,"percentage":100,"fresh_air_on":true,"fresh_air_speed":75})",
      true, iotcloud::HubDeviceBus::Origin::Local);
  assert(relay.incomingBemfa("fan003", "on#2", false));
  assert(!deserializeJson(command, std::get<1>(local.back())) &&
         command["action"] == "fresh_air.set" &&
         command["params"]["speed"] == 50);
  bus.publish(fresh + "/state", R"({"fan_on":true,"percentage":100})",
                 true, iotcloud::HubDeviceBus::Origin::Local);
  assert(!relay.incomingBemfa("fan003", "off", false));
  relay.reset();
  DynamicJsonDocument advertisement(2048);
  advertisement["schema"]=2;advertisement["protocol"]="tmcp";advertisement["protocol_version"]=2;advertisement["version"]="v2";advertisement["model"]="a1";advertisement["device_id"]="aabbccddeeff";advertisement["firmware_version"]="1.0-rc.4";advertisement["variant"]="standard";advertisement["wire_version"]=1;advertisement["default_wire_format"]="msgpack";advertisement.createNestedArray("wire_formats").add("msgpack");
  std::string packed;serializeMsgPack(advertisement,packed);iotcloud::hubWire.observe(service.prefix()+"discovery/aabbccddeeff",packed);assert(iotcloud::hubWire.binary(fresh));
  assert(!deserializeJson(state,R"({"fan_on":true,"percentage":100,"fresh_air_on":true,"fresh_air_speed":75})"));packed.clear();serializeMsgPack(state,packed);
  bus.publish(fresh+"/state",packed,true,iotcloud::HubDeviceBus::Origin::Local);
  assert(relay.incomingBemfa("fan003","off",false));
  assert(!deserializeMsgPack(command,std::get<1>(local.back()))&&command["action"]=="fresh_air.set"&&command["params"]["on"]==false);
  state.clear();state["request_id"]=command["request_id"];state["code"]="accepted";packed.clear();serializeMsgPack(state,packed);
  bus.publish(fresh+"/result",packed,false,iotcloud::HubDeviceBus::Origin::Local);assert(relay.status=="connected");
  puts("hub bridge/Bemfa tests passed");
}
