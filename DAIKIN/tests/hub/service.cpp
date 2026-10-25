#include "hub_service.h"
#include <cassert>
#include <cstdio>
#include <openssl/evp.h>
#include <openssl/rand.h>
static std::string hex(const uint8_t *bytes, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += "0123456789abcdef"[bytes[i] >> 4];
    s += "0123456789abcdef"[bytes[i] & 15];
  }
  return s;
}
int main() {
  DynamicJsonDocument config(16384);
  config["auth"]["password"] = "device-web-password";
  iotcloud::HubService service(config);
  bool fail = false;
  unsigned writes = 0, drops = 0;
  service.store = [&](DynamicJsonDocument &next) {
    if (fail)
      return false;
    config.set(next);
    writes++;
    return true;
  };
  service.changed = [&](bool credentials) { drops += credentials; };
  service.randomHex = [](size_t n) {
    uint8_t b[32];
    assert(n <= sizeof(b) && RAND_bytes(b, n) == 1);
    return hex(b, n);
  };
  service.derive = [](const std::string &password, const std::string &salt) {
    uint8_t hash[32];
    assert(PKCS5_PBKDF2_HMAC(password.data(), password.size(),
                             (const unsigned char *)salt.data(), salt.size(),
                             10000, EVP_sha256(), sizeof(hash), hash));
    return hex(hash, sizeof(hash));
  };
  using Request = iotcloud::HubService::Request;
  assert(
      service
          .handle(Request("GET", "/api/register/status", "", "", "192.168.1.2"),
                  0)
          .body.find("mqtt://192.168.1.2:1883") != std::string::npos);
  auto setup = Request("POST", "/api/hub/setup",
                       R"({"username":"local","password":"local-password"})");
  assert(service.handle(setup, 0).status == 403 && writes == 0);
  setup.setupAuthorized = true;
  fail = true;
  assert(service.handle(setup, 0).status == 503 && !service.ready() &&
         writes == 0);
  fail = false;
  assert(service.handle(setup, 0).status == 201 && service.ready() &&
         writes == 1);
  assert(service.handle(setup, 0).status == 409 && writes == 1);
  assert(iotcloud::HubService::valid(config["hub"]));
  assert(service.authenticate("local", "local-password", 1));
  assert(!service.authenticate("cloud", "local-password", 1));
  auto login = service.handle(
      Request("POST", "/api/auth/login",
              R"({"username":"local","password":"local-password"})"),
      2);
  assert(login.status == 200 && login.token.size() == 64);
  std::string token = login.token;
  DynamicJsonDocument response(1024);
  assert(!deserializeJson(response, login.body) &&
         response["username"] == "local" && response["is_admin"] == false);
  assert(service.handle(Request("GET", "/api/auth/me", "", token), 3).status ==
         200);
  // HomeKit belongs to the locally authenticated owner. Setup authorization,
  // another hub's session, and cloud credentials do not authorize its API.
  {
    unsigned statusCalls = 0, resetCalls = 0, homeChanges = 0;
    bool resetSucceeds = false;
    service.homeStatus = [&](JsonObject out) {
      statusCalls++;
      out["ready"] = true;
      out["setup_code"] = "123-45-679";
    };
    service.homeReset = [&] { resetCalls++; return resetSucceeds; };
    auto changed = service.changed;
    service.changed = [&](bool credentials) {
      homeChanges++;
      changed(credentials);
    };
    auto snapshot = [&] {
      std::string json;
      serializeJson(config, json);
      return json;
    };
    const std::string home = "/api/hub/homekit", pairing = home + "/pairing";
    DynamicJsonDocument foreignConfig(16384);
    foreignConfig.set(config);
    foreignConfig["hub"]["username"] = "foreign";
    iotcloud::HubService foreign(foreignConfig);
    foreign.derive = service.derive;
    foreign.randomHex = service.randomHex;
    auto foreignLogin = foreign.handle(Request("POST", "/api/auth/login",
        R"({"username":"foreign","password":"local-password"})"), 3);
    assert(foreignLogin.status == 200);
    auto beforeConfig = snapshot();
    unsigned beforeWrites = writes, beforeDrops = drops;
    for (const auto &key : {std::string(), std::string(64, 'x'), foreignLogin.token}) {
      assert(service.handle(Request("GET", home, "", key), 3).status == 401);
      assert(service.handle(Request("PUT", home, R"({"enabled":true})", key), 3).status == 401);
      assert(service.handle(Request("DELETE", pairing, R"({"confirm":true})", key), 3).status == 401);
    }
    assert(service.handle(Request("PUT", home, R"({"enabled":true})", "", "", true), 3).status == 401);
    assert(statusCalls == 0 && resetCalls == 0 && writes == beforeWrites &&
           homeChanges == 0 && snapshot() == beforeConfig);
    auto status = service.handle(Request("GET", home, "", token), 3);
    DynamicJsonDocument homeReply(1024);
    assert(status.status == 200 && !deserializeJson(homeReply, status.body) &&
           homeReply["enabled"] == false && homeReply["protocol"] == "hap" &&
           homeReply["setup_code"] == "123-45-679" && statusCalls == 1);
    for (const char *body : {"{}", R"({"enabled":1})", R"({"enabled":"true"})",
         R"({"enabled":null})", R"({"enabled":true,"username":"foreign"})",
         R"({"enabled":true,"instance":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})",
         R"({"enabled":true,"setup_code":"123-45-679"})", "[]", "{"})
      assert(service.handle(Request("PUT", home, body, token), 3).status == 400);
    assert(service.handle(Request("PATCH", home, "{}", token), 3).status == 400);
    assert(writes == beforeWrites && homeChanges == 0 && snapshot() == beforeConfig);
    const std::string instance = config["hub"]["instance"].as<std::string>();
    fail = true;
    auto enable = Request("PUT", home, R"({"enabled":true})", token);
    assert(service.handle(enable, 3).status == 503 && writes == beforeWrites &&
           homeChanges == 0 && snapshot() == beforeConfig);
    fail = false;
    auto accepted = service.handle(enable, 3);
    assert(accepted.status == 202 && !deserializeJson(homeReply, accepted.body) &&
           homeReply["restart_required"] == true &&
           config["hub"]["homekit"]["enabled"] == true &&
           config["hub"]["instance"] == instance && homeChanges == 1 &&
           drops == beforeDrops);
    beforeConfig = snapshot();
    beforeWrites = writes;
    auto disable = Request("PUT", home, R"({"enabled":false})", token);
    fail = true;
    assert(service.handle(disable, 3).status == 503 && snapshot() == beforeConfig &&
           homeChanges == 1 && writes == beforeWrites);
    fail = false;
    assert(service.handle(disable, 3).status == 202 &&
           config["hub"]["homekit"]["enabled"] == false && homeChanges == 2);
    beforeConfig = snapshot();
    beforeWrites = writes;
    for (const char *body : {"", "{}", R"({"confirm":false})", R"({"confirm":1})",
         R"({"confirm":"true"})", R"({"confirm":true,"all":true})", "[]"})
      assert(service.handle(Request("DELETE", pairing, body, token), 3).status == 400);
    assert(service.handle(Request("GET", pairing, "", token), 3).status == 400);
    assert(service.handle(Request("PUT", pairing, R"({"confirm":true})", token), 3).status == 400);
    assert(resetCalls == 0);
    auto reset = Request("DELETE", pairing, R"({"confirm":true})", token);
    auto resetHandler = service.homeReset;
    service.homeReset = {};
    assert(service.handle(reset, 3).status == 503);
    service.homeReset = resetHandler;
    assert(service.handle(reset, 3).status == 503 && resetCalls == 1);
    resetSucceeds = true;
    accepted = service.handle(reset, 3);
    assert(accepted.status == 202 && !deserializeJson(homeReply, accepted.body) &&
           homeReply["restart_required"] == true && resetCalls == 2);
    assert(snapshot() == beforeConfig && writes == beforeWrites &&
           homeChanges == 2 && drops == beforeDrops);
    assert(service.handle(Request("GET", "/api/auth/me", "", token), 3).status == 200);
    // Existing accounts receive an identity only after a successful save.
    DynamicJsonDocument legacyConfig(16384);
    legacyConfig.set(config);
    legacyConfig["hub"].remove("instance");
    iotcloud::HubService legacy(legacyConfig);
    legacy.derive = service.derive;
    legacy.randomHex = service.randomHex;
    auto legacyLogin = legacy.handle(Request("POST", "/api/auth/login",
        R"({"username":"local","password":"local-password"})"), 3);
    assert(legacyLogin.status == 200);
    bool legacyFail = true;
    unsigned legacyChanges = 0;
    legacy.store = [&](DynamicJsonDocument &next) {
      if (legacyFail) return false;
      legacyConfig.set(next);
      return true;
    };
    legacy.changed = [&](bool) { legacyChanges++; };
    auto legacyEnable = Request("PUT", home, R"({"enabled":true})", legacyLogin.token);
    legacy.randomHex = [](size_t) { return std::string(32, 'x'); };
    assert(legacy.handle(legacyEnable, 3).status == 503);
    legacy.randomHex = service.randomHex;
    assert(legacy.handle(legacyEnable, 3).status == 503 && legacyChanges == 0 &&
           legacyConfig["hub"]["instance"].isNull() &&
           legacyConfig["hub"]["homekit"]["enabled"] == false);
    legacyFail = false;
    assert(legacy.handle(legacyEnable, 3).status == 202 && legacyChanges == 1 &&
           iotcloud::HubService::hex(legacyConfig["hub"]["instance"], 32));
    service.changed = changed;
    service.homeStatus = {};
    service.homeReset = {};
  }
  auto add =
      Request("POST", "/api/devices",
              R"({"mac":"aabbccddeeff","model":"tc1","version":"v2"})", token);
  assert(service.handle(add, 3).status == 201);
  assert(service.handle(add, 3).status == 409);
  assert(service.handle(Request("GET", "/api/devices", "", token), 4)
             .body.find("aabbccddeeff") != std::string::npos);
  auto bemfa = Request(
      "PUT", "/api/integrations/bemfa",
      R"({"enabled":true,"api_key":"0123456789abcdef","mappings":[{"device":{"mac":"aabbccddeeff","model":"tc1","version":"v2","status":"active","username":"local"},"channel":0,"topic":"socket001"}]})",
      token);
  auto saved = service.handle(bemfa, 5);
  assert(saved.status == 200 &&
         saved.body.find("0123456789abcdef") == std::string::npos);
  assert(config["hub"]["bemfa"]["api_key"] == "0123456789abcdef");
  assert(
      service
          .handle(Request("DELETE", "/api/devices/aabbccddeeff", "", token), 6)
          .status == 200);
  assert(config["hub"]["bemfa"]["mappings"].size() == 0 &&
         config["hub"]["bemfa"]["enabled"] == false);
  auto bridge = Request(
      "PUT", "/api/hub/bridge",
      R"({"enabled":true,"mqtt":{"host":"broker.lan","port":1883,"user":"cloud","password":"upstream-secret"},"mappings":[{"direction":"in","local_topic":"home/status","remote_topic":"upstream/status","retain":true,"allow_retained":true}]})",
      token);
  saved = service.handle(bridge, 7);
  assert(saved.status == 200 &&
         saved.body.find("upstream-secret") == std::string::npos);
  assert(config["mqtt"]["password"] == "upstream-secret");
  bridge.body =
      R"({"enabled":true,"mappings":[{"direction":"in","local_topic":"phicomm/v2/u_6f74686572/m1/aabbccddeeff/command","remote_topic":"upstream/set","retain":false,"allow_retained":true}]})";
  assert(service.handle(bridge, 8).status == 400);
  // Failed storage leaves credentials, sessions, routes and the running broker
  // alone.
  fail = true;
  unsigned before = drops;
  auto change = Request(
      "POST", "/api/auth/mqtt-password",
      R"({"current_password":"local-password","password":"mqtt-password"})",
      token);
  assert(service.handle(change, 9).status == 503 &&
         service.authenticate("local", "local-password", 10) &&
         drops == before);
  assert(service.handle(Request("GET", "/api/auth/me", "", token), 10).status ==
         200);
  fail = false;
  assert(service.handle(change, 11).status == 200 && drops == before + 1);
  assert(service.authenticate("local", "mqtt-password", 12) &&
         !service.authenticate("local", "local-password", 12));
  assert(service.handle(Request("GET", "/api/auth/me", "", token), 12).status ==
         200);
  for (unsigned i = 0; i < 5; i++)
    service.handle(Request("POST", "/api/auth/login",
                           R"({"username":"local","password":"wrong"})"),
                   20 + i);
  assert(service
             .handle(
                 Request("POST", "/api/auth/login",
                         R"({"username":"local","password":"local-password"})"),
                 26)
             .status == 429);
  login = service.handle(
      Request("POST", "/api/auth/login",
              R"({"username":"local","password":"local-password"})"),
      31026);
  assert(login.status == 200);
  assert(service
             .handle(Request("GET", "/api/auth/me", "", login.token),
                     43200001 + 31026)
             .status == 401);
  assert(service.handle(Request("GET", "/api/hub/homekit", "", login.token),
                        43200001 + 31026).status == 401);
  // A foreign/new schema cannot be silently restored or treated as no account.
  DynamicJsonDocument corrupt(16384);
  corrupt.set(config);
  corrupt["hub"]["schema"] = 2;
  assert(!iotcloud::HubService::valid(corrupt["hub"]));
  corrupt.set(config);
  corrupt["hub"]["mqtt_hash"] = "invalid";
  assert(!iotcloud::HubService::valid(corrupt["hub"]));
  puts("hub account/configuration tests passed");
}
