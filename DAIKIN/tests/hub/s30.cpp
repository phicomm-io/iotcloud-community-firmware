/* Host business/model acceptance; real UDP interop is a separate fixture. */
#include "hub_home.h"
#include "hub_home_budget.h"
#include "hub_lan_discovery.h"
#include "hub_relay.h"
#include <cassert>
#include <cstdio>
#include <openssl/sha.h>
#include <tuple>

using Home = iotcloud::HubHome;
using Service = iotcloud::HubService;
using Origin = iotcloud::HubDeviceBus::Origin;
static const char *id = "02c400000001", *own = "02c400000002";
static const std::string prefix = "phicomm/v2/u_6f776e6572/";
static const std::string base = prefix + "s30-v1b/" + id;

static std::string pack(const JsonDocument &doc) {
  std::string bytes;
  serializeMsgPack(doc, bytes);
  return bytes;
}
static std::string pack(const char *json) {
  DynamicJsonDocument doc(2048);
  assert(!deserializeJson(doc, json));
  return pack(doc);
}
static void advertise(iotcloud::HubWire &wire, const std::string &model,
                      const std::string &mac) {
  DynamicJsonDocument doc(2048);
  assert(!deserializeJson(doc, R"({"schema":2,"protocol":"tmcp","protocol_version":2,"version":"v2","variant":"standard","firmware_version":"1.0.0-rc.4","wire_version":1,"default_wire_format":"msgpack","wire_formats":["msgpack"]})"));
  doc["device_id"] = mac;
  doc["model"] = model;
  wire.observe(prefix + "discovery/" + mac, pack(doc));
}
static void discovery() {
  iotcloud::HubLanDiscovery lan;
  const std::map<std::string, std::string> good{{"id", id}, {"model", "s30-v1b"},
      {"variant", "BL2028N"}, {"firmware", "1.0.0-rc.4-c11e58-dirty"},
      {"lan_wire", "4"}, {"codec", "msgpack"}};
  assert(lan.observe(good, "192.0.2.1", 11288, 1));
  auto peer = lan.find(id, 2);
  assert(peer && peer->model == "s30-v1b" && peer->variant == "BL2028N");
  for (const auto *key : {"id", "model", "variant", "firmware", "lan_wire", "codec"}) {
    auto bad = good;
    bad.erase(key);
    assert(!lan.observe(bad, "192.0.2.1", 11288, 3));
  }
  for (const auto &entry : std::vector<std::pair<std::string, std::string>>{
      {"id", "000000000000"}, {"id", "ffffffffffff"}, {"id", "02C400000001"},
      {"model", "S30-V1B"}, {"model", "s30"}, {"model", "bk7231n"},
      {"lan_wire", "2"}, {"codec", "json"}, {"variant", std::string(33, 'x')},
      {"firmware", std::string(65, 'x')}, {"variant", "bad\nvariant"}}) {
    auto bad = good;
    bad[entry.first] = entry.second;
    assert(!lan.observe(bad, "192.0.2.1", 11288, 3));
  }
  assert(!lan.observe(good, "", 11288, 3));
  assert(!lan.observe(good, "192.0.2.1", 0, 3));
  assert(!lan.find(id, 90001));
  for (const auto *model : {"tc1", "a1", "m1", "daikin"}) {
    auto txt = good;
    txt["model"] = model;
    assert(lan.observe(txt, "192.0.2.1", 11288, 4));
    iotcloud::HubWire wire;
    advertise(wire, model, id);
    assert(wire.binary(prefix + model + "/" + id));
  }
  iotcloud::HubWire wire;
  DynamicJsonDocument legacy(1024);
  assert(!deserializeJson(legacy, R"({"schema":1,"protocol":"tmcp","protocol_version":2,"version":"v2","model":"s30-v1b","device_id":"02c400000001"})"));
  std::string text;
  serializeJson(legacy, text);
  wire.observe(prefix + "discovery/" + id, text);
  assert(wire.binary(base)); // The new model has no default legacy JSON codec.
  DynamicJsonDocument state(512);
  assert(wire.decode(state, base, R"({"socket_states":[true]})"));
  assert(!wire.decode(state, base, pack(R"({"socket_states":[true]})")));
  advertise(wire, "s30-v1b", id);
  assert(wire.binary(base));
  wire.observe(prefix + "discovery/" + id, text);
  assert(wire.binary(base)); // Legacy discovery cannot downgrade the new model.
}

static std::string account(Service &service) {
  using Request = Service::Request;
  auto setup = Request("POST", "/api/hub/setup",
      R"({"username":"owner","password":"owner-password"})", "", "", true);
  assert(service.handle(setup, 1).status == 201);
  auto login = service.handle(Request("POST", "/api/auth/login",
      R"({"username":"owner","password":"owner-password"})"), 2);
  assert(login.status == 200 && login.token.size() == 64);
  return login.token;
}
static void registration(DynamicJsonDocument &config, Service &service) {
  service.store = [&](DynamicJsonDocument &next) { config = std::move(next); return true; };
  service.randomHex = [](size_t n) { return std::string(n * 2, 'a'); };
  service.derive = [](const std::string &password, const std::string &salt) {
    const std::string input = password + salt;
    uint8_t digest[32];
    SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest);
    std::string out;
    for (uint8_t c : digest) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    return out;
  };
  const auto token = account(service);
  using Request = Service::Request;
  const std::string device = R"({"mac":"02c400000001","model":"s30-v1b","version":"v2"})";
  assert(service.handle(Request("POST", "/api/devices", device), 3).status == 401);
  assert(config["hub"]["devices"].size() == 0);
  assert(service.handle(Request("POST", "/api/devices", device, token), 3).status == 201);
  assert(config["hub"]["devices"][0]["model"] == "s30-v1b");
  assert(Service::deviceValid(config["hub"]["devices"][0]));
  assert(Service::valid(config["hub"]));
  assert(service.handle(Request("POST", "/api/devices", device, token), 3).status == 409);
  DynamicJsonDocument bad(512);
  bad.set(config["hub"]["devices"][0]);
  for (const auto *model : {"s30", "S30-V1B", "bl2028n", "bk7231n"}) {
    bad["model"] = model;
    assert(!Service::deviceValid(bad.as<JsonObjectConst>()));
  }
  for (const auto *model : {"tc1", "a1", "m1", "daikin"}) {
    bad["model"] = model;
    assert(Service::deviceValid(bad.as<JsonObjectConst>()));
  }
  auto bemfa = config["hub"].createNestedObject("bemfa");
  bemfa["enabled"] = true;
  bemfa["api_key"] = "0123456789abcdef";
  auto mapping = bemfa.createNestedArray("mappings").createNestedObject();
  mapping["device"] = config["hub"]["devices"][0];
  mapping["channel"] = 0;
  mapping["topic"] = "s30socket001";
  assert(Service::valid(config["hub"]));
  for (unsigned channel : {1u, 2u, 5u, 6u, 999u}) {
    mapping["channel"] = channel;
    assert(!Service::valid(config["hub"]));
  }
  mapping["channel"] = 0;
  for (const auto *topic : {"s30socket002", "s30socket003"}) {
    mapping["topic"] = topic;
    assert(!Service::valid(config["hub"]));
  }
  mapping["topic"] = "s30socket001";
  mapping["device"]["mac"] = "02c400000099";
  assert(!Service::valid(config["hub"])); // Discovery/mapping does not register a peer.
  mapping["device"]["mac"] = id;
  assert(Service::valid(config["hub"]));
}

static std::vector<std::pair<size_t, unsigned>> graphOutlets;
static unsigned graphSensors, graphFans, graphLights;
struct GraphObject { virtual ~GraphObject() = default; };
static std::vector<GraphObject *> graphObjects;
struct HomeOutlet : GraphObject { HomeOutlet(size_t d, unsigned c) { graphOutlets.emplace_back(d, c); graphObjects.push_back(this); } };
struct HomeSensor : GraphObject { HomeSensor(size_t, Home::Metric) { ++graphSensors; graphObjects.push_back(this); } };
struct HomeFan : GraphObject { explicit HomeFan(size_t) { ++graphFans; graphObjects.push_back(this); } };
struct HomeLight : GraphObject { explicit HomeLight(size_t) { ++graphLights; graphObjects.push_back(this); } };
static void homeAccessory(uint32_t, const char *, const char *, const char *) {}
static void buildGraph(const Home &hubHome) {
#include "home-device-graph.inc"
}
static void home(DynamicJsonDocument &config) {
  iotcloud::HubDeviceBus bus;
  Home home(bus);
  assert(home.configure(config["hub"]["devices"], prefix, own));
  assert(home.size == 2 && home.devices[1].model == "s30-v1b" &&
         home.devices[1].outletCount() == 1 && home.devices[0].outletCount() == 0);
  buildGraph(home);
  assert(graphOutlets.size() == 1 && graphOutlets[0].first == 1 && graphOutlets[0].second == 0);
  assert(graphSensors == 5 && !graphFans && !graphLights); // Only own DAIKIN sensors.
  for (auto object : graphObjects) delete object;
  graphObjects.clear();
  advertise(iotcloud::hubWire, "s30-v1b", id);
  unsigned serial = 0, sends = 0;
  home.randomHex = [&](size_t n) { auto s = std::to_string(++serial); return std::string(n * 2 - s.size(), '0') + s; };
  std::string body;
  bus.dispatch = [&](const std::string &route, const std::string &bytes, bool retain, Origin origin) {
    assert(route == base + "/command" && !retain && origin == Origin::Internal);
    body = bytes; ++sends; return true;
  };
  home.tick(100);
  const auto off = pack(R"({"model":"s30-v1b","device_id":"02c400000001","socket_states":[false]})");
  const auto on = pack(R"({"model":"s30-v1b","device_id":"02c400000001","socket_states":[true]})");
  home.observe(base + "/state", off);
  assert(home.devices[1].controls && home.devices[1].reachable(100) && !home.devices[1].outlets[0]);
  for (unsigned channel : {1u, 2u, 5u, 6u, 999u})
    assert(!home.begin(1, Home::Outlet, channel, 1));
  assert(!home.begin(1, Home::FanOn, 0, 1) && !home.begin(1, Home::Outlet, 0, 2) && !sends);
  for (const auto *invalid : {R"({"socket_states":[]})", R"({"socket_states":[true,false]})",
       R"({"socket_states":[1]})", R"({"socket_states":[null]})", R"({"socket_states":["true"]})",
       R"({"socket_states":[true,false,false,false,false,false]})"}) {
    const auto sequence = home.devices[1].sequence;
    home.observe(base + "/state", pack(invalid));
    assert(!home.devices[1].controls && home.devices[1].sequence == sequence && !home.devices[1].outlets[0]);
    assert(!home.begin(1, Home::Outlet, 0, 1));
    home.observe(base + "/state", off);
  }
  const auto sequence = home.devices[1].sequence;
  home.observe(base + "/state", pack(R"({"model":"tc1","socket_states":[true]})"));
  home.observe(base + "/state", pack(R"({"device_id":"02c400000099","socket_states":[true]})"));
  assert(home.devices[1].sequence == sequence && !home.devices[1].outlets[0]);
  assert(home.begin(1, Home::Outlet, 0, 1));
  DynamicJsonDocument request(512), reply(1024);
  assert(!deserializeMsgPack(request, body) && request["action"] == "outlet.set" &&
         request["params"]["index"] == 0 && request["params"]["on"] == true);
  reply["request_id"] = request["request_id"]; reply["code"] = "accepted";
  home.observe(base + "/result", pack(reply));
  assert(!home.finished() && !home.devices[1].outlets[0]);
  assert(!deserializeMsgPack(request, body) && request["action"] == "status.get");
  reply.clear(); reply["request_id"] = request["request_id"]; reply["code"] = "ok";
  auto data = reply.createNestedObject("data"); data["model"] = "s30-v1b";
  data["device_id"] = id; data.createNestedArray("socket_states").add(true);
  home.observe(base + "/result", pack(reply));
  assert(home.finished() && home.finish() && home.devices[1].outlets[0]);
  for (unsigned i = 1; i < 6; ++i) assert(!home.devices[1].outlets[i]);
  home.observe(base + "/availability", "offline");
  const auto sent = sends;
  assert(!home.begin(1, Home::Outlet, 0, 0) && sends == sent);
  home.observe(base + "/state", on);
  assert(!home.devices[1].reachable(100));
}

static void relay(DynamicJsonDocument &config, Service &service) {
  iotcloud::HubDeviceBus bus;
  iotcloud::HubRelay relay(config, service, bus);
  using Event = std::tuple<std::string, std::string, bool>;
  std::vector<Event> local, remote, bemfa;
  bus.dispatch = [&](const std::string &topic, const std::string &body, bool retain, Origin origin) {
    local.emplace_back(topic, body, retain); relay.local(topic, body, retain, origin); return true;
  };
  relay.upstream = [&](const std::string &t, const std::string &b, bool r) { remote.emplace_back(t, b, r); return true; };
  relay.bemfa = [&](const std::string &t, const std::string &b, bool r) { bemfa.emplace_back(t, b, r); return true; };
  relay.randomHex = [](size_t n) { return std::string(n * 2, 'b'); };
  relay.tick(100);
  const std::string routeBase = service.prefix() + "s30-v1b/" + id;
  advertise(iotcloud::hubWire, "s30-v1b", id);
  assert(routeBase == base);
  auto bridge = config["hub"].createNestedObject("bridge"); bridge["enabled"] = true;
  auto mappings = bridge.createNestedArray("mappings");
  for (const auto *direction : {"out", "in"}) {
    auto r = mappings.createNestedObject(); r["direction"] = direction;
    r["local_topic"] = routeBase + (std::string(direction) == "out" ? "/state" : "/command");
    r["remote_topic"] = std::string("upstream/") + direction;
    r["retain"] = false; r["allow_retained"] = false;
  }
  assert(Service::valid(config["hub"]));
  assert(!relay.incomingBemfa("s30socket001", "off", false));
  const auto on = pack(R"({"model":"s30-v1b","device_id":"02c400000001","socket_states":[true]})");
  bus.publish(routeBase + "/state", on, false, Origin::Local);
  assert(bemfa.size() == 1 && std::get<1>(bemfa.back()) == "on");
  assert(remote.size() == 1 && std::get<0>(remote.back()) == "upstream/out" && std::get<1>(remote.back()) == on);
  assert(!relay.incomingBemfa("s30socket001", "off", true));
  assert(!relay.incomingBemfa("s30socket001", "on#1", false));
  assert(relay.incomingBemfa("s30socket001", "off", false));
  DynamicJsonDocument request(512), reply(512);
  assert(!deserializeMsgPack(request, std::get<1>(local.back())) &&
         request["action"] == "outlet.set" && request["params"]["index"] == 0 && request["params"]["on"] == false);
  reply["request_id"] = request["request_id"]; reply["code"] = "accepted";
  bus.publish(routeBase + "/result", pack(reply), false, Origin::Internal);
  assert(relay.status == "connected");
  const auto mutation = pack(R"({"action":"outlet.set","request_id":"bridge","params":{"index":0,"on":false}})");
  const auto before = local.size();
  relay.incoming("upstream/in", mutation, true); assert(local.size() == before);
  relay.incoming("upstream/in", mutation, false);
  assert(local.size() == before + 1 && std::get<0>(local.back()) == routeBase + "/command" &&
         std::get<1>(local.back()) == mutation && !std::get<2>(local.back()));
  auto mapping = config["hub"]["bemfa"]["mappings"][0].as<JsonObject>();
  DynamicJsonDocument output(512), state(512);
  assert(!deserializeMsgPack(state, on));
  for (unsigned channel : {1u, 2u, 5u, 6u, 999u}) {
    mapping["channel"] = channel; output.clear();
    assert(!iotcloud::HubRelay::command(mapping, "on", output));
    assert(iotcloud::HubRelay::report(mapping, state.as<JsonObjectConst>()).empty());
  }
  mapping["channel"] = 0;
  for (const auto *bad : {R"({"socket_states":[1]})", R"({"socket_states":[true,false]})",
       R"({"model":"tc1","socket_states":[true]})"}) {
    assert(!deserializeJson(state, bad));
    assert(iotcloud::HubRelay::report(mapping, state.as<JsonObjectConst>()).empty());
    bus.publish(routeBase + "/state", pack(state), false, Origin::Local);
    assert(!relay.incomingBemfa("s30socket001", "off", false));
  }
}

int main() {
  discovery();
  DynamicJsonDocument config(16384);
  Service service(config);
  registration(config, service);
  home(config);
  relay(config, service);
  std::puts("PASS S30-V1B discovery/owner registration, strict bool single outlet, measured HAP graph/confirmation, bridge/Bemfa on-off and channel rejection; existing four model whitelist retained");
}
