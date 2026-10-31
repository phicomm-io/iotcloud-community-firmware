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
  config.shrinkToFit();
  iotcloud::HubService service(config);
  bool fail = false;
  size_t candidateLimit = iotcloud::HubService::ConfigCapacity;
  unsigned writes = 0, drops = 0;
  service.store = [&](DynamicJsonDocument &next) {
    assert(next.capacity() <= candidateLimit);
    if (fail)
      return false;
    config = std::move(next);
    config.shrinkToFit();
    writes++;
    return true;
  };
  service.changed = [&](bool credentials) { drops += credentials; };
  service.randomHex = [](size_t n) {
    uint8_t b[32];
    assert(n <= sizeof(b) && RAND_bytes(b, n) == 1);
    return hex(b, n);
  };
  using Request = iotcloud::HubService::Request;
  {
    DynamicJsonDocument openConfig(1024);
    iotcloud::HubService open(openConfig);
    open.randomHex=service.randomHex;
    open.store=[&](DynamicJsonDocument &next){openConfig=std::move(next);return true;};
    assert(open.initializeIdentity()&&open.user()=="admin");
    assert(openConfig["auth"]["password"].isNull()&&openConfig["hub"]["schema"]==2);
    assert(open.handle(Request("GET","/api/devices"),1).status==401);
    assert(open.handle(Request("GET","/api/devices","",true),1).status==200);
    assert(open.initializeIdentity());
  }
  // A valid legacy namespace migrates atomically, preserving nested business
  // settings and the independent device Web/broker credentials.
  config=DynamicJsonDocument(16384);
  config["auth"]["password"]="device-web-password";
  config["mqtt"]["password"]="upstream-password";
  auto old=config.createNestedObject("hub");old["schema"]=1;old["username"]="local";
  old["salt"]=std::string(32,'1');old["password_hash"]=std::string(64,'2');
  old["mqtt_salt"]=std::string(32,'3');old["mqtt_hash"]=std::string(64,'4');
  old["mqtt_password_separate"]=true;old["instance"]=std::string(32,'5');old.createNestedArray("devices");
  old["homekit"]["enabled"]=false;
  std::string before;serializeJson(config,before);fail=true;
  assert(!service.initializeIdentity()&&writes==0);
  std::string after;serializeJson(config,after);assert(before==after);
  fail=false;assert(service.initializeIdentity()&&service.ready()&&writes==1&&drops==0);
  assert(config["hub"]["schema"]==2&&service.user()=="local"&&
         config["hub"]["instance"]==std::string(32,'5')&&config["hub"]["homekit"]["enabled"]==false);
  for(const char *key:{"salt","password_hash","mqtt_salt","mqtt_hash","mqtt_password_separate"})assert(!config["hub"].containsKey(key));
  assert(config["auth"]["password"]=="device-web-password"&&config["mqtt"]["password"]=="upstream-password");
  assert(service.initializeIdentity()&&writes==1&&iotcloud::HubService::valid(config["hub"]));
  assert(service.handle(Request("GET","/api/register/status"),0).body.find("\"setup_required\":false")!=std::string::npos);
  const bool token=true;
  DynamicJsonDocument response(1024);
  auto me=service.handle(Request("GET","/api/auth/me","",token),3);
  assert(me.status==200&&!deserializeJson(response,me.body)&&response["username"]=="local"&&response["auth"]=="webui");
  // Derived LAN inventory is owner-only and carries no transport credentials.
  {
    const std::string path="/api/hub/lan";
    unsigned calls=0;
    auto empty=service.handle(Request("GET",path,"",token),3);
    DynamicJsonDocument lan(16384);assert(empty.status==200&&!deserializeJson(lan,empty.body));
    assert(lan["protocol"]==4&&lan["codec"]=="msgpack"&&lan["discovery"]=="mdns");
    assert(lan["ready"]==false&&lan["devices"].size()==0);
    service.lanStatus=[&](JsonObject out){
      ++calls;out["ready"]=true;auto rows=out["devices"].as<JsonArray>();
      for(unsigned i=0;i<iotcloud::HubService::Devices;++i){
        auto d=rows.createNestedObject();d["device_id"]="aabbccddeeff";d["model"]="a1";
        d["variant"]="standard";d["firmware"]="1.0.0-rc.4";d["ip"]="192.0.2.1";d["port"]=11288;
        d["compatible"]=i!=0;d["registered"]=i!=0;d["paired"]=i!=0;d["online"]=true;
        d["code"]=i?"ok":"incompatible";
      }
    };
    assert(service.handle(Request("GET",path,"",false),3).status==401);
    assert(calls==0);
    auto full=service.handle(Request("GET",path,"",token),3);
    assert(full.status==200&&!deserializeJson(lan,full.body)&&calls==1);
    assert(lan["ready"]==true&&lan["devices"].size()==iotcloud::HubService::Devices);
    auto unknown=lan["devices"][0];assert(unknown["compatible"]==false&&unknown["paired"]==false);
    assert(unknown["device_id"]=="aabbccddeeff"&&unknown["model"]=="a1"&&unknown["variant"]=="standard");
    assert(unknown["firmware"]=="1.0.0-rc.4"&&unknown["ip"]=="192.0.2.1"&&unknown["port"]==11288);
    assert(unknown["registered"]==false&&unknown["online"]==true&&unknown["code"]=="incompatible");
    assert(full.body.find("password")==std::string::npos&&full.body.find("token")==std::string::npos);
    for(const auto &method:{"POST","PUT","DELETE"})
      assert(service.handle(Request(method,path,"{}",token),3).status==405);
    assert(calls==1);service.lanStatus=nullptr;
    auto advertised=service.handle(Request("GET","/api/register/status","","","192.168.1.2"),3);
    assert(advertised.body.find("mqtt_uri")==std::string::npos&&advertised.body.find("mqtt_ws_uri")==std::string::npos);
  }
  // Owner token authorizes LAN pairing separately from device-local Web tokens.
  {
    const std::string pair="/api/hub/lan/pair",unpair="/api/hub/lan/unpair",status=pair+"/aabbccddeeff";
    const std::string valid=R"({"peer":"aabbccddeeff","ip":"192.0.2.1","password":"pair-secret","scopes":1})";
    unsigned calls=0;std::string lastOp;
    std::string statusPhase="paired",statusCode="ok";
    assert(service.handle(Request("POST",pair,valid,token),3).status==503);
    service.lanOperation=[&](const std::string &op,JsonObjectConst in,JsonObject out)->const char*{
      ++calls;lastOp=op;assert(in["peer"]=="aabbccddeeff");
      if(op=="pair"){assert(in["password"]=="pair-secret"&&in["scopes"]==1);out["phase"]="queued";return "accepted";}
      if(op=="status"){out["phase"]=statusPhase;out["code"]=statusCode;return "ok";}
      assert(op=="unpair");return "accepted";
    };
    assert(service.handle(Request("POST",pair,valid),3).status==401);
    assert(service.handle(Request("GET",status),3).status==401);
    for(const auto &bad:{
      R"({"peer":"wrong","ip":"192.0.2.1","password":"pair-secret","scopes":1})",
      R"({"peer":"aabbccddeeff","ip":"999.0.2.1","password":"pair-secret","scopes":1})",
      R"({"peer":"aabbccddeeff","ip":"1..2.1","password":"pair-secret","scopes":1})",
      R"({"peer":"aabbccddeeff","ip":"192.0.2.1","password":"pair-secret","scopes":2})",
      R"({"peer":"aabbccddeeff","ip":"192.0.2.1","password":"123456789012345678901234567890123","scopes":1})",
      R"({"peer":"aabbccddeeff","ip":"192.0.2.1","password":"pair-secret","scopes":1,"extra":true})"})
      assert(service.handle(Request("POST",pair,bad,token),3).status==400);
    assert(service.handle(Request("POST",unpair,R"({"peer":"aabbccddeeff","extra":1})",token),3).status==400);
    assert(service.handle(Request("GET",pair+"/unknown","",token),3).status==400);
    assert(calls==0);
    auto accepted=service.handle(Request("POST",pair,valid,token),3);
    DynamicJsonDocument pairAccepted(256);
    assert(accepted.status==202&&lastOp=="pair"&&calls==1&&!deserializeJson(pairAccepted,accepted.body));
    assert(pairAccepted["code"]=="accepted");
    assert(accepted.body.find("pair-secret")==std::string::npos&&accepted.body.find("password")==std::string::npos);
    auto current=service.handle(Request("GET",status,"",token),3);
    DynamicJsonDocument pairStatus(512);
    assert(current.status==200&&lastOp=="status"&&!deserializeJson(pairStatus,current.body));
    assert(pairStatus["phase"]=="paired"&&pairStatus["code"]=="ok"&&pairStatus["operation_code"]=="ok");
    statusPhase="failed";statusCode="storage_error";
    current=service.handle(Request("GET",status,"",token),3);
    assert(current.status==200&&!deserializeJson(pairStatus,current.body));
    assert(pairStatus["phase"]=="failed"&&pairStatus["code"]=="storage_error"&&pairStatus["operation_code"]=="ok");
    statusPhase="paired";statusCode="ok";
    assert(service.handle(Request("POST",unpair,R"({"peer":"aabbccddeeff"})",token),3).status==202&&lastOp=="unpair");
    const unsigned before=calls;
    assert(service.handle(Request("DELETE",pair,valid,token),3).status==405&&calls==before);
    auto invalidAction=service.handle(Request("GET",unpair,"",token),3);
    assert(invalidAction.status==405&&invalidAction.body.find("method_not_allowed")!=std::string::npos&&calls==before);
    for(const auto &code:{"busy","invalid_params","auth_required","migration_required","storage_error","unexpected"}){
      service.lanOperation=[&](const std::string &,JsonObjectConst,JsonObject)->const char*{return code;};
      auto result=service.handle(Request("POST",pair,valid,token),3);
      const int expected=std::string(code)=="invalid_params"?400:std::string(code)=="auth_required"?403:std::string(code)=="migration_required"?409:std::string(code)=="unexpected"?500:503;
      assert(result.status==expected);
      DynamicJsonDocument errorReply(256);
      const char *expectedCode=std::string(code)=="unexpected"?"internal_error":code;
      assert(!deserializeJson(errorReply,result.body)&&errorReply["code"]==expectedCode);
    }
    service.lanOperation=nullptr;
  }
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
    auto beforeConfig = snapshot();
    unsigned beforeWrites = writes, beforeDrops = drops;
    assert(service.handle(Request("GET", home, "", false), 3).status == 401);
    assert(service.handle(Request("PUT", home, R"({"enabled":true})", false), 3).status == 401);
    assert(service.handle(Request("DELETE", pairing, R"({"confirm":true})", false), 3).status == 401);
    assert(service.handle(Request("PUT", home, R"({"enabled":true})", false), 3).status == 401);
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
    legacy.randomHex = service.randomHex;
    bool legacyFail = true;
    unsigned legacyChanges = 0;
    legacy.store = [&](DynamicJsonDocument &next) {
      if (legacyFail) return false;
      legacyConfig.set(next);
      return true;
    };
    legacy.changed = [&](bool) { legacyChanges++; };
    auto legacyEnable = Request("PUT", home, R"({"enabled":true})", true);
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
  // Saving the initial disabled form must keep an absent key empty, rather
  // than converting ArduinoJson's null value to the string "null".
  {
    assert(config["hub"]["bemfa"].isNull());
    auto initial = service.handle(
        Request("GET", "/api/integrations/bemfa", "", token), 5);
    assert(initial.status == 200 && !deserializeJson(response, initial.body) &&
           response["enabled"] == false && response["key_set"] == false &&
           response["mappings"].is<JsonArray>() &&
           response["mappings"].size() == 0 && !response.containsKey("api_key"));
    const unsigned beforeWrites = writes;
    auto disabled = service.handle(Request("PUT", "/api/integrations/bemfa",
        R"({"enabled":false,"api_key":"","mappings":[]})", token), 5);
    if (disabled.status != 200)
      fprintf(stderr, "initial disabled Bemfa save: HTTP %d %s\n",
              disabled.status, disabled.body.c_str());
    assert(disabled.status == 200 && writes == beforeWrites + 1 &&
           config["hub"]["bemfa"]["api_key"] == "" &&
           config["hub"]["bemfa"]["enabled"] == false &&
           config["hub"]["bemfa"]["mappings"].size() == 0 &&
           !deserializeJson(response, disabled.body) &&
           response["key_set"] == false && !response.containsKey("api_key"));
  }
  auto bemfa = Request(
      "PUT", "/api/integrations/bemfa",
      R"({"enabled":true,"api_key":"0123456789abcdef","mappings":[{"device":{"mac":"aabbccddeeff","model":"tc1","version":"v2","status":"active","username":"local"},"channel":0,"topic":"socket001"}]})",
      token);
  {
    std::string beforeConfig;
    serializeJson(config, beforeConfig);
    auto unchanged = [&] {
      std::string current;
      serializeJson(config, current);
      return current == beforeConfig;
    };
    const unsigned beforeWrites = writes, beforeDrops = drops;
    auto noKey = bemfa;
    noKey.body.erase(noKey.body.find("0123456789abcdef"), 16);
    // A real mapping isolates the absent-key check from the empty-map check.
    assert(service.handle(noKey, 5).status == 400 &&
           writes == beforeWrites && unchanged());
    fail = true;
    auto rejected = service.handle(bemfa, 5);
    assert(rejected.status == 503 &&
           rejected.body.find("storage_error") != std::string::npos &&
           writes == beforeWrites && drops == beforeDrops && unchanged());
    assert(service.handle(Request("GET", "/api/auth/me", "", token), 5).status == 200);
    fail = false;
  }
  auto saved = service.handle(bemfa, 5);
  assert(saved.status == 200 &&
         saved.body.find("0123456789abcdef") == std::string::npos);
  assert(config["hub"]["bemfa"]["api_key"] == "0123456789abcdef");
  // Keeping a masked API key must copy it before the old config pool is freed.
  auto keepKey = bemfa;
  const auto keyAt = keepKey.body.find("0123456789abcdef");
  assert(keyAt != std::string::npos);
  keepKey.body.erase(keyAt, 16);
  auto kept = service.handle(keepKey, 5);
  assert(kept.status == 200 &&
         config["hub"]["bemfa"]["api_key"] == "0123456789abcdef" &&
         !deserializeJson(response, kept.body) && response["key_set"] == true &&
         !response.containsKey("api_key") &&
         kept.body.find("0123456789abcdef") == std::string::npos);
  auto masked = service.handle(
      Request("GET", "/api/integrations/bemfa", "", token), 5);
  assert(masked.status == 200 && !deserializeJson(response, masked.body) &&
         response["key_set"] == true && !response.containsKey("api_key") &&
         masked.body.find("0123456789abcdef") == std::string::npos);
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
  // A real HAP session left only a 7924-byte contiguous block. Small route
  // edits and their rollback must fit it repeatedly without clearing accounts,
  // devices or the session. Keep the maximum-size configuration tests below.
  candidateLimit = 7924;
  const Request disableBridge("PUT", "/api/hub/bridge",
      R"({"enabled":false,"mappings":[]})", token);
  for (unsigned i = 0; i < 16; ++i) {
    assert(service.largestAllocation(bridge) <= candidateLimit);
    assert(service.handle(bridge, 7).status == 200);
    assert(service.largestAllocation(disableBridge) <= candidateLimit);
    assert(service.handle(disableBridge, 7).status == 200);
    assert(config["hub"]["bridge"]["enabled"] == false &&
           config["hub"]["bridge"]["mappings"].size() == 0 &&
           config["mqtt"]["password"] == "upstream-secret" &&
           service.handle(Request("GET", "/api/auth/me", "", token), 7).status == 200);
  }
  candidateLimit = iotcloud::HubService::ConfigCapacity;
  bridge.body =
      R"({"enabled":true,"mappings":[{"direction":"in","local_topic":"phicomm/v2/u_6f74686572/m1/aabbccddeeff/command","remote_topic":"upstream/set","retain":false,"allow_retained":true}]})";
  assert(service.handle(bridge, 8).status == 400);
  // Response strings must fit the checked contiguous allocation, including
  // repeated long values and JSON escaping that expands their wire length.
  for (bool escaped : {false, true}) {
    DynamicJsonDocument responseConfig(16384);
    responseConfig.set(config);
    responseConfig["hub"].remove("bridge");
    auto longBridge = responseConfig["hub"].createNestedObject("bridge");
    longBridge["enabled"] = true;
    auto routes = longBridge.createNestedArray("mappings");
    const std::string remote(escaped ? 70 : 480, escaped ? '\x01' : 'r');
    for (unsigned i = 0; i < 8; ++i) {
      auto route = routes.createNestedObject();
      route["direction"] = "out";
      route["local_topic"] = "local/" + std::to_string(i);
      route["remote_topic"] = remote;
      route["retain"] = false;
      route["allow_retained"] = false;
    }
    const std::string host(escaped ? 127 : 10, escaped ? '\x01' : 'h');
    responseConfig["mqtt"]["host"] = host;
    responseConfig["mqtt"]["user"] = std::string(escaped ? 63 : 5, escaped ? '\x01' : 'u');
    assert(!responseConfig.overflowed() && measureJson(responseConfig) <= 6144 &&
           iotcloud::HubService::valid(responseConfig["hub"]));
    responseConfig.shrinkToFit();
    iotcloud::HubService responseService(responseConfig);
    responseService.randomHex = service.randomHex;
    const Request query("GET", "/api/hub/bridge", "", true);
    auto result = responseService.handle(query, 8);
    DynamicJsonDocument decoded(16384);
    assert(result.status == 200 && !deserializeJson(decoded, result.body) &&
           decoded["mappings"].size() == 8 &&
           decoded["mappings"][7]["remote_topic"].as<std::string>() == remote &&
           decoded["mqtt"]["host"].as<std::string>() == host);
    assert(result.body.capacity() + 1 <= responseService.largestAllocation(query));
  }
  // A compact live pool must still allow the complete device capacity to grow.
  for (unsigned i = 1; i <= 12; ++i) {
    char id[13];
    snprintf(id, sizeof(id), "%012x", i);
    auto body = std::string("{\"mac\":\"") + id +
                "\",\"model\":\"m1\",\"version\":\"v2\"}";
    assert(service.handle(Request("POST", "/api/devices", body, token), 8).status == 201);
    assert(!config.overflowed() && config.capacity() < iotcloud::HubService::ConfigCapacity);
  }
  auto listing = service.handle(Request("GET", "/api/devices", "", token), 8);
  DynamicJsonDocument devices(8192);
  assert(listing.status == 200 && !deserializeJson(devices, listing.body) && devices.size() == 12);
  // The hub itself is runtime identity: visible even with all 12 user slots
  // occupied, without changing stored registrations or making a flash write.
  service.localMac = "9c9c1ff76748";
  unsigned selfWrites = writes;
  const auto savedDevices = config["hub"]["devices"].size();
  listing = service.handle(Request("GET", "/api/devices", "", token), 8);
  assert(listing.status == 200 && !deserializeJson(devices, listing.body) && devices.size() == 13);
  assert(devices[0]["mac"] == service.localMac && devices[0]["model"] == "daikin" &&
         devices[0]["self"] == true && devices[0]["username"] == "local");
  assert(service.handle(Request("DELETE", "/api/devices/9c9c1ff76748", "", token), 8).status == 403);
  assert(service.handle(Request("POST", "/api/devices",
      R"({"mac":"9c9c1ff76748","model":"daikin","version":"v2"})", token), 8).status == 409);
  assert(config["hub"]["devices"].size() == savedDevices && writes == selfWrites);
  // Existing manually registered self rows must not produce duplicate cards.
  config["hub"]["devices"][0]["mac"] = "9c9c1ff76748";
  listing = service.handle(Request("GET", "/api/devices", "", token), 8);
  assert(!deserializeJson(devices, listing.body) && devices.size() == 12 && devices[0]["self"] == true);
  config["hub"]["devices"][0]["mac"] = "000000000001";
  {
    // Exercise the response pool and wire budget with the maximum owner name
    // repeated across all twelve saved devices, plus the automatic local row.
    DynamicJsonDocument maximum(16384);
    maximum.set(config);
    const char *owner = "abcdefghijklmnopqrstuvwxyz012345";
    maximum["hub"]["username"] = owner;
    for (JsonObject d : maximum["hub"]["devices"].as<JsonArray>()) {
      d["username"] = owner;
      d["model"] = "daikin";
    }
    assert(!maximum.overflowed() && iotcloud::HubService::valid(maximum["hub"]));
    iotcloud::HubService large(maximum);
    large.localMac = service.localMac;
    large.randomHex = service.randomHex;
    auto all = large.handle(Request("GET", "/api/devices", "", true), 8);
    assert(all.status == 200 && !deserializeJson(devices, all.body) && devices.size() == 13);
    for (JsonObjectConst d : devices.as<JsonArrayConst>()) assert(d["username"] == owner);
  }
  // Obsolete independent credentials have no mutation path.
  const unsigned unchanged=writes;
  const auto webPassword=config["auth"]["password"].as<std::string>();
  const auto mqttPassword=config["mqtt"]["password"].as<std::string>();
  for(const char *path:{"/api/hub/setup","/api/auth/login","/api/auth/password","/api/auth/mqtt-password","/api/auth/logout"})
    assert(service.handle(Request("POST",path,"{}",true),9).status==404);
  assert(writes==unchanged&&config["auth"]["password"]==webPassword&&config["mqtt"]["password"]==mqttPassword);
  assert(service.handle(Request("GET","/api/auth/me","",false),10).status==401);
  // A foreign/new schema cannot be silently restored or treated as no account.
  DynamicJsonDocument corrupt(16384);
  corrupt.set(config);
  corrupt["hub"]["schema"] = 3;
  assert(!iotcloud::HubService::valid(corrupt["hub"]));
  corrupt.set(config);
  corrupt["hub"]["mqtt_hash"] = "invalid";
  assert(!iotcloud::HubService::valid(corrupt["hub"]));
  puts("hub account/configuration tests passed");
}
