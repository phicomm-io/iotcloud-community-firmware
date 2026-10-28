#include <algorithm>
#include <cstdlib>
#include <stdlib.h>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <string>
// Exercise the pinned ArduinoJson implementation with an actual allocation
// ceiling, including its NoMemory result. Other C++/bus allocations are not
// capped here; they are covered by the byte-memory bus suite.
static size_t jsonLimit=SIZE_MAX,jsonAttempts=0,jsonDenied=0,jsonLargest=0;
static size_t jsonLive=0,jsonPeak=0;
static void *homeJsonAllocate(size_t n){
  ++jsonAttempts;jsonLargest=std::max(jsonLargest,n);
  if(n>jsonLimit){++jsonDenied;return nullptr;}
  auto p=std::malloc(n);if(p)jsonPeak=std::max(jsonPeak,++jsonLive);return p;
}
static void homeJsonFree(void *p){if(p){assert(jsonLive);--jsonLive;}std::free(p);}
static void *homeJsonReallocate(void *p,size_t n){
  if(n>jsonLimit){++jsonDenied;return nullptr;}
  auto q=std::realloc(p,n);if(q&&!p)jsonPeak=std::max(jsonPeak,++jsonLive);return q;
}
#define malloc homeJsonAllocate
#define free homeJsonFree
#define realloc homeJsonReallocate
#include <ArduinoJson.h>
#undef malloc
#undef free
#undef realloc
#include "hub_home.h"

static std::string dense(const std::string &object,unsigned entries){
  assert(!object.empty()&&object.back()=='}');
  auto result=object.substr(0,object.size()-1)+",\"ignored\":[";
  for(unsigned i=0;i<entries;++i)result+=i?",0":"0";
  return result+"]}";
}
static void memoryTests(){
  using Home=iotcloud::HubHome;
  DynamicJsonDocument saved(1024);
  assert(!deserializeJson(saved,R"([{"mac":"112233445566","model":"m1"},{"mac":"aabbccddeeff","model":"tc1"}])"));
  iotcloud::HubDeviceBus bus;Home home(bus);
  unsigned serial=0;
  home.randomHex=[&](size_t n){auto s=std::to_string(++serial);return std::string(n*2-s.size(),'0')+s;};
  assert(home.configure(saved.as<JsonArrayConst>(),"lab/","aabbcc000001"));
  std::string command;
  bus.dispatch=[&](const std::string &,const std::string &body,bool,iotcloud::HubDeviceBus::Origin){command=body;return true;};
  auto observe=[&](const std::string &topic,const std::string &body,size_t ceiling){
    const auto live=jsonLive;jsonLimit=ceiling;jsonAttempts=jsonDenied=jsonLargest=0;jsonPeak=live;
    home.observe(topic,body);jsonLimit=SIZE_MAX;
    assert(jsonLive==live&&jsonPeak<=live+1); // Retries never retain old pools.
  };
  const auto sensor=home.devices[1].base,control=home.devices[2].base;
  const std::string m1=R"({"model":"m1","device_id":"112233445566","sensor_valid":true,"age_ms":1200,"climate_valid":true,"pm25_valid":true,"temperature":25.5,"humidity":43,"pm25":12,"scd40":{"valid":true,"co2_ppm":720,"age_ms":500}})";
  home.tick(50000);observe(sensor+"/state",m1,2548);
  const auto &device=home.devices[1];
  assert(device.reachable(50000)&&device.values[Home::Temperature].number==25.5&&device.values[Home::Temperature].fresh(50000));
  assert(device.values[Home::Humidity].number==43&&device.values[Home::PM25].number==12&&device.values[Home::CO2].number==720);
  assert(jsonAttempts==1&&!jsonDenied&&jsonLargest<=2548);
  home.tick(51000);observe(sensor+"/state",dense(m1,180),SIZE_MAX);
  assert(device.at==51000&&device.sequence==2&&jsonAttempts>1&&!jsonDenied&&jsonLargest<=8192);
  const auto sequence=device.sequence,at=device.at,sampleAt=device.values[Home::Temperature].at;
  auto unchanged=[&](){assert(device.sequence==sequence&&device.at==at&&device.values[Home::Temperature].at==sampleAt&&device.values[Home::Temperature].number==25.5&&device.online);};
  home.tick(52000);observe(sensor+"/state",dense(m1,180),2548);
  assert(jsonAttempts>1&&jsonDenied==1);unchanged();
  observe(sensor+"/state",m1,0);assert(jsonDenied==1);unchanged();
  // A complete wire object may still exceed the old pool limit. Neither that
  // failure nor a syntax error at the end may partially refresh known values.
  observe(sensor+"/state",dense(m1,300),SIZE_MAX);
  assert(jsonAttempts>1&&!jsonDenied&&jsonLargest==8192);unchanged();
  auto malformed=dense(m1,180);malformed.pop_back();
  observe(sensor+"/state",malformed,SIZE_MAX);unchanged();
  const std::string off=R"({"socket_states":[0,0,0,0,0,0]})",on=R"({"socket_states":[1,0,0,0,0,0]})";
  observe(control+"/state",off,2548);assert(home.begin(2,Home::Outlet,0,1));
  DynamicJsonDocument request(512);assert(!deserializeJson(request,command));
  const auto receipt="{\"request_id\":\""+request["request_id"].as<std::string>()+"\",\"code\":\"accepted\"}";
  const auto sent=command;
  observe(control+"/result",dense(receipt,180),2548);
  assert(jsonDenied==1&&command==sent&&!home.finished());
  observe(control+"/state",on,2548);assert(!home.finished());
  observe(control+"/result",receipt,2548);assert(!home.finished());
  const auto controlSequence=home.devices[2].sequence;
  observe(control+"/state",dense(on,180),2548);
  assert(jsonDenied==1&&home.devices[2].sequence==controlSequence&&!home.finished());
  observe(control+"/state",on,2548);assert(home.finished()&&home.finish());
  puts("hub home fragmented JSON pool, retry and control-confirmation tests passed");
}
int main() {
  memoryTests();
  DynamicJsonDocument saved(2048);
  assert(!deserializeJson(saved,
      R"([{"mac":"aabbccddeeff","model":"tc1"},{"mac":"112233445566","model":"m1"},{"mac":"a1a2a3a4a5a6","model":"a1"}])"));
  iotcloud::HubDeviceBus bus;
  iotcloud::HubHome home(bus);
  using Home = iotcloud::HubHome;
  const std::string prefix = "phicomm/v2/u_6c6f63616c/";
  unsigned serial = 0;
  home.randomHex = [&](size_t n) {
    auto s = std::to_string(++serial);
    return std::string(n * 2 - s.size(), '0') + s;
  };
  assert(home.configure(saved.as<JsonArrayConst>(), prefix, "aabbcc000001"));
  assert(home.size == 4 && home.devices[1].aid != home.devices[2].aid);
  auto aid = home.devices[1].aid;
  std::string topic, body;
  bus.dispatch = [&](const std::string &t, const std::string &b, bool retain,
                     iotcloud::HubDeviceBus::Origin) {
    assert(!retain);
    topic = t;
    body = b;
    return true;
  };
  home.tick(100);
  assert(!home.begin(1, Home::Outlet, 0, 1));
  const auto base = home.devices[1].base;
  const char *off = R"({"model":"tc1","device_id":"AABBCCDDEEFF","socket_states":[0,0,0,0,0,0]})";
  const char *on = R"({"model":"tc1","device_id":"AABBCCDDEEFF","socket_states":[1,0,0,0,0,0]})";
  home.observe("phicomm/v2/u_6f74686572/tc1/aabbccddeeff/state", on);
  assert(!home.devices[1].seen);
  home.observe(base + "/state", off);
  assert(home.devices[1].controls && home.devices[1].reachable(100));
  assert(!home.begin(1, Home::Outlet, 6, 1));
  assert(home.begin(1, Home::Outlet, 0, 1));
  DynamicJsonDocument cmd(768);
  assert(!deserializeJson(cmd, body));
  assert(topic == base + "/command" && cmd["action"] == "outlet.set" &&
         cmd["params"]["index"] == 0 && cmd["params"]["on"] == true);
  std::string receipt = "{\"request_id\":\"" +
                        cmd["request_id"].as<std::string>() +
                        "\",\"code\":\"accepted\"}";
  home.observe(base + "/result", R"({"request_id":"other","code":"ok"})");
  home.observe(base + "/state", on);
  assert(!home.finished());
  home.observe(base + "/result", receipt);
  assert(!home.finished()); // A snapshot preceding the receipt cannot confirm it.
  assert(!deserializeJson(cmd, body) && cmd["action"] == "status.get");
  home.observe(base + "/state", on);
  assert(home.finished() && home.finish());
  assert(home.devices[1].outlets[0]);
  // A receipt alone never turns the desired value into reported state.
  assert(home.begin(1, Home::Outlet, 0, 0));
  assert(!deserializeJson(cmd, body));
  receipt = "{\"request_id\":\"" + cmd["request_id"].as<std::string>() +
            "\",\"code\":\"ok\"}";
  home.observe(base + "/result", receipt);
  assert(!home.finished() && !home.finish() && home.devices[1].outlets[0]);
  home.observe(base + "/availability", "offline");
  home.observe(base + "/state", off);
  assert(!home.devices[1].reachable(101));
  home.observe(base + "/availability", "online");
  home.observe(base + "/state", off);
  assert(home.devices[1].reachable(101));
  home.tick(30101);
  assert(!home.begin(1, Home::Outlet, 0, 1));
  home.refresh();
  assert(!deserializeJson(cmd, body) && cmd["action"] == "status.get");
  // Invalid or cross-device bodies cannot overwrite a valid state.
  home.observe(base + "/state", R"({"model":"a1","socket_states":[1,1,1,1,1,1]})");
  assert(!home.devices[1].outlets[0]);
  home.tick(50000);
  const auto sensor = home.devices[2].base;
  home.observe(sensor + "/state",
      R"({"model":"m1","sensor_valid":true,"age_ms":1200,"climate_valid":true,"pm25_valid":true,"temperature":25.5,"humidity":43,"pm25":12,"scd40":{"valid":true,"co2_ppm":720,"age_ms":500}})");
  assert(home.devices[2].values[Home::Temperature].fresh(68000));
  assert(!home.devices[2].values[Home::Temperature].fresh(69000));
  assert(home.devices[2].values[Home::CO2].number == 720);
  assert(!home.begin(2, Home::FanOn, 0, 1));
  home.observe(sensor + "/state",
      R"({"sensor_valid":false,"age_ms":21000,"climate_valid":false,"pm25_valid":false,"temperature":0,"humidity":0,"pm25":0})");
  assert(!home.devices[2].values[Home::Temperature].valid &&
         !home.devices[2].values[Home::CO2].valid);
  const auto daikin = home.devices[0].base;
  home.observe(daikin + "/state",
      R"({"sensors":{"co2":{"valid":true,"value":800,"unit":"ppm","age_ms":20},"temperature":{"valid":true,"value":22,"unit":"F","age_ms":20}}})");
  assert(home.devices[0].values[Home::CO2].valid &&
         !home.devices[0].values[Home::Temperature].valid);
  const auto fan = home.devices[3].base;
  home.observe(fan + "/state",
      R"({"fan_on":true,"percentage":50,"led_visible":true,"brightness":128})");
  assert(home.begin(3, Home::LightBrightness, 0, 50));
  assert(!deserializeJson(cmd, body) && cmd["action"] == "light.set" &&
         cmd["params"]["brightness"] == 50);
  auto acknowledge = [&](const char *code = "accepted") {
    std::string id = cmd["request_id"].as<std::string>();
    home.observe(fan + "/result", "{\"request_id\":\"" + id +
                                      "\",\"code\":\"" + code + "\"}");
  };
  acknowledge();
  assert(!home.finished());
  home.observe(fan + "/state",
      R"({"fan_on":true,"percentage":50,"led_visible":true,"brightness":128})");
  assert(home.finished() && home.finish());
  assert(!home.begin(3, Home::LightBrightness, 0, 101));
  assert(!home.begin(3, Home::FanSpeed, 0, 50.5));
  assert(!home.begin(3, Home::FanSpeed, 0, std::nan("")));
  assert(!home.begin(3, Home::Control(99), 0, 1));
  assert(!home.begin(3, Home::FanOn, 0, 1, true, false));
  // The normal fan still uses the ordinary action, with a percent level.
  assert(home.begin(3, Home::FanSpeed, 0, 60));
  assert(!deserializeJson(cmd, body) && cmd["action"] == "fan.set" &&
         cmd["params"]["on"] == true && cmd["params"]["speed"] == 60);
  acknowledge("busy");
  assert(home.finished() && !home.finish());
  // One batch mutation keeps explicit off dominant over a positive level.
  assert(home.begin(3, Home::LightBrightness, 0, 75, true, false));
  assert(!deserializeJson(cmd, body) && cmd["action"] == "light.set" &&
         cmd["params"]["enabled"] == false &&
         cmd["params"]["brightness"] == 75);
  acknowledge();
  home.observe(fan + "/state",
      R"({"fan_on":true,"percentage":50,"led_visible":true,"brightness":191})");
  assert(!home.finished());
  home.observe(fan + "/state",
      R"({"fan_on":true,"percentage":50,"led_visible":false,"brightness":191})");
  assert(home.finished() && home.finish());
  // Fresh-air fans are identified by applied fields, never the requested
  // fields or the primary fan that can be running independently.
  home.observe(fan + "/state",
      R"({"fan_on":true,"percentage":80,"fresh_air_on":false,"fresh_air_speed":0,"fresh_air_requested_on":true,"fresh_air_requested_speed":90,"led_visible":false,"brightness":191})");
  assert(home.devices[3].freshAir && !home.devices[3].fan &&
         home.devices[3].speed == 0);
  assert(home.begin(3, Home::FanOn, 0, 0));
  assert(!deserializeJson(cmd, body) && cmd["action"] == "fresh_air.set" &&
         cmd["params"]["on"] == false);
  acknowledge();
  home.observe(fan + "/state",
      R"({"fan_on":false,"percentage":0,"fresh_air_on":true,"fresh_air_speed":40,"fresh_air_requested_on":false,"fresh_air_requested_speed":0,"led_visible":false,"brightness":191})");
  assert(!home.finished());
  home.observe(fan + "/state",
      R"({"fan_on":true,"percentage":80,"fresh_air_on":false,"fresh_air_speed":0,"fresh_air_requested_on":true,"fresh_air_requested_speed":90,"led_visible":false,"brightness":191})");
  assert(home.finished() && home.finish());
  assert(home.begin(3, Home::FanSpeed, 0, 60));
  assert(!deserializeJson(cmd, body) && cmd["action"] == "fresh_air.set" &&
         cmd["params"]["on"] == true && cmd["params"]["speed"] == 60 &&
         cmd["params"]["mode"] == "manual");
  acknowledge();
  home.observe(fan + "/state",
      R"({"fan_on":true,"percentage":60,"fresh_air_on":true,"fresh_air_speed":40,"fresh_air_requested_on":true,"fresh_air_requested_speed":60,"led_visible":false,"brightness":191})");
  assert(!home.finished());
  home.observe(fan + "/state",
      R"({"fan_on":true,"percentage":10,"fresh_air_on":true,"fresh_air_speed":60,"fresh_air_requested_on":true,"fresh_air_requested_speed":5,"led_visible":false,"brightness":191})");
  assert(home.finished() && home.finish());
  assert(home.begin(3, Home::FanSpeed, 0, 80, true, false));
  assert(!deserializeJson(cmd, body) && cmd["action"] == "fresh_air.set" &&
         cmd["params"]["on"] == false && cmd["params"]["speed"] == 80);
  acknowledge();
  // A post-receipt status.get response also supplies a fresh confirmation.
  assert(!deserializeJson(cmd, body) && cmd["action"] == "status.get");
  home.observe(fan + "/result", "{\"request_id\":\"" +
      cmd["request_id"].as<std::string>() +
      R"(","code":"ok","data":{"fresh_air_on":false,"fresh_air_speed":0,"led_visible":false,"brightness":191}})");
  assert(home.finished() && home.finish());
  // A malformed/missing fresh-air output cannot downgrade a known variant
  // and silently control its primary fan.
  home.observe(fan + "/state",
      R"({"fan_on":true,"percentage":50,"led_visible":true,"brightness":128})");
  assert(home.devices[3].freshAir && !home.devices[3].controls &&
         !home.begin(3, Home::FanOn, 0, 1));
  // Clock wrap still expires samples and control state correctly.
  home.tick(0xfffffff0u);
  home.observe(base + "/state", off);
  assert(home.devices[1].reachable(10));
  assert(!home.devices[1].reachable(40000));
  assert(home.configure(saved.as<JsonArrayConst>(), prefix, "aabbcc000001") &&
         home.devices[1].aid == aid && !home.devices[1].seen);
  // A discovered MP device follows exactly the same acknowledged-control
  // contract, including an allocation-free MP confirmation status query.
  DynamicJsonDocument advertisement(2048),binaryState(1024);
  advertisement["schema"]=2;advertisement["protocol"]="tmcp";advertisement["protocol_version"]=2;advertisement["version"]="v2";advertisement["model"]="tc1";advertisement["device_id"]="aabbccddeeff";advertisement["firmware_version"]="1.0-rc.4";advertisement["variant"]="standard";advertisement["wire_version"]=1;advertisement["default_wire_format"]="msgpack";advertisement.createNestedArray("wire_formats").add("msgpack");
  std::string packed;serializeMsgPack(advertisement,packed);iotcloud::hubWire.observe(prefix+"discovery/aabbccddeeff",packed);assert(iotcloud::hubWire.binary(base));
  assert(!deserializeJson(binaryState,off));packed.clear();serializeMsgPack(binaryState,packed);home.tick(100);home.observe(base+"/state",packed);assert(home.begin(1,Home::Outlet,0,1));
  assert(!deserializeMsgPack(cmd,body)&&cmd["action"]=="outlet.set"&&cmd["params"]["index"]==0&&cmd["params"]["on"]==true);
  binaryState.clear();binaryState["request_id"]=cmd["request_id"];binaryState["code"]="accepted";packed.clear();serializeMsgPack(binaryState,packed);home.observe(base+"/result",packed);
  assert(!deserializeMsgPack(cmd,body)&&cmd["action"]=="status.get"&&cmd["params"].is<JsonObject>()&&cmd["params"].size()==0);
  home.observe(base+"/state",on);assert(!home.finished());
  assert(!deserializeJson(binaryState,on));packed.clear();serializeMsgPack(binaryState,packed);home.observe(base+"/state",packed);assert(home.finished()&&home.finish());
  puts("hub home bridge model tests passed");
}
