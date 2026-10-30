// SPDX-License-Identifier: GPL-3.0-or-later
// Direct application bus simulation. No MQTT sockets or UDP wire acceptance.
#include "hub_home.h"
#include "hub_relay.h"
#include <cassert>
#include <cstdio>
#include <deque>
static DynamicJsonDocument config(16384);
static iotcloud::HubDeviceBus bus;
static iotcloud::HubService hubService(config);
static iotcloud::HubHome hubHome(bus);
static iotcloud::HubRelay hubRelay(config,hubService,bus);
static uint32_t clockMs=100;
static void pump(){++clockMs;hubRelay.tick(clockMs);}
struct Client;
static std::vector<Client*> clients;
struct Client{
 std::string base;bool binary;
 std::vector<std::string> routes;
 std::deque<std::pair<std::string,std::string>> inbox;
 Client(const std::string&model,const std::string&mac,bool mp):base(hubService.prefix()+model+"/"+mac),binary(mp){clients.push_back(this);routes.push_back(base+"/command");}
 void publish(const std::string&topic,const std::string&body,bool retain=false){assert(bus.publish(topic,body,retain,iotcloud::HubDeviceBus::Origin::Local));pump();}
 std::string encode(const JsonDocument&doc){std::string body;if(binary)serializeMsgPack(doc,body);else serializeJson(doc,body);return body;}
 void state(const std::string&json){DynamicJsonDocument d(4096);assert(!deserializeJson(d,json));publish(base+"/state",encode(d),true);}
 void subscribe(const std::string&topic){routes.push_back(topic);}
 std::string payload(const std::string&topic){for(auto it=inbox.begin();it!=inbox.end();++it)if(it->first==topic){auto body=it->second;inbox.erase(it);return body;}assert(false);return "";}
 void command(JsonDocument&doc){auto body=payload(base+"/command");assert(!(binary?deserializeMsgPack(doc,body):deserializeJson(doc,body)));assert(body[0]==(binary?char(0x83):'{'));}
 void result(const JsonDocument&command,const char*code="accepted"){DynamicJsonDocument reply(512);reply["request_id"]=command["request_id"];reply["code"]=code;publish(base+"/result",encode(reply));}
 void discover(const std::string&model,const std::string&mac,bool invalid=false){DynamicJsonDocument d(2048);d["schema"]=binary?2:1;d["protocol"]="tmcp";d["protocol_version"]=2;d["model"]=model;if(binary){d["device_id"]=invalid?"000000000000":mac;d["firmware_version"]="1.0.0-rc.4";d["variant"]="standard";d["version"]="v2";d["wire_version"]=1;d["default_wire_format"]="msgpack";d.createNestedArray("wire_formats").add("msgpack");}publish(hubService.prefix()+"discovery/"+mac,encode(d),true);}
};
int main(){assert(!deserializeJson(config,R"({"hub":{"username":"local","devices":[{"mac":"aabbccdd0001","model":"tc1","version":"v2","status":"active","username":"local"},{"mac":"aabbccdd0002","model":"a1","version":"v2","status":"active","username":"local"},{"mac":"aabbccdd0003","model":"m1","version":"v2","status":"active","username":"local"}],"bemfa":{"enabled":true,"api_key":"fixture","mappings":[{"device":{"mac":"aabbccdd0001","model":"tc1","version":"v2","username":"local"},"channel":0,"topic":"old001"},{"device":{"mac":"aabbccdd0002","model":"a1","version":"v2","username":"local"},"channel":0,"topic":"new003"}]},"bridge":{"enabled":true,"mappings":[{"direction":"out","local_topic":"lab/out","remote_topic":"cloud/out","retain":false},{"direction":"in","local_topic":"lab/in","remote_topic":"cloud/in","retain":false}]}}})"));
 bus.dispatch=[](const std::string&topic,const std::string&body,bool retain,iotcloud::HubDeviceBus::Origin origin){
 if(origin==iotcloud::HubDeviceBus::Origin::Local||origin==iotcloud::HubDeviceBus::Origin::Internal){iotcloud::hubWire.observe(topic,body);hubHome.tick(clockMs);hubHome.observe(topic,body);}
 hubRelay.local(topic,body,retain,origin);
 for(auto c:clients)for(const auto&r:c->routes)if(r==topic)c->inbox.emplace_back(topic,body);
 return true;
 };
 unsigned serial=0;auto random=[&](size_t n){auto s=std::to_string(++serial);return std::string(n*2-s.size(),'0')+s;};hubHome.randomHex=random;hubRelay.randomHex=random;assert(hubHome.configure(config["hub"]["devices"],hubService.prefix(),"aabbccdd9999"));std::vector<std::string> reports,upstream;hubRelay.bemfa=[&](const std::string&t,const std::string&b,bool){reports.push_back(t+":"+b);return true;};hubRelay.upstream=[&](const std::string&t,const std::string&b,bool){upstream.push_back(t+":"+b);return true;};
 Client old("tc1","aabbccdd0001",false),modern("a1","aabbccdd0002",true),sensor("m1","aabbccdd0003",true);
 old.discover("tc1","aabbccdd0001");modern.discover("a1","aabbccdd0002");sensor.discover("m1","aabbccdd0003");assert(!iotcloud::hubWire.binary(old.base)&&iotcloud::hubWire.binary(modern.base)&&iotcloud::hubWire.binary(sensor.base));modern.discover("a1","aabbccdd0002",true);assert(iotcloud::hubWire.binary(modern.base));
 const char*off=R"({"socket_states":[0,0,0,0,0,0]})",*on=R"({"socket_states":[1,0,0,0,0,0]})";old.state(off);modern.state(R"({"fan_on":true,"percentage":40,"led_visible":true,"brightness":128})");sensor.state(R"({"sensor_valid":true,"climate_valid":true,"pm25_valid":true,"age_ms":10,"temperature":25.5,"humidity":40,"pm25":12,"scd40":{"valid":true,"co2_ppm":650,"age_ms":10}})");assert(hubHome.devices[1].controls&&hubHome.devices[2].controls&&hubHome.devices[3].values[iotcloud::HubHome::Temperature].number==25.5);
 DynamicJsonDocument command(1024);hubHome.tick(clockMs);assert(hubHome.begin(1,iotcloud::HubHome::Outlet,0,1));old.command(command);assert(command["action"]=="outlet.set");old.state(on);assert(!hubHome.finished());old.result(command);old.command(command);assert(command["action"]=="status.get");old.state(on);assert(hubHome.finished()&&hubHome.finish());
 hubHome.tick(clockMs);assert(hubHome.begin(2,iotcloud::HubHome::FanSpeed,0,75));modern.command(command);assert(command["action"]=="fan.set"&&command["params"]["speed"]==75);modern.result(command);modern.command(command);assert(command["action"]=="status.get");auto sequence=hubHome.devices[2].sequence;modern.publish(modern.base+"/state",R"({"fan_on":true,"percentage":75,"led_visible":true,"brightness":128})");assert(hubHome.devices[2].sequence==sequence&&!hubHome.finished());modern.state(R"({"fan_on":true,"percentage":75,"led_visible":true,"brightness":128})");assert(hubHome.finished()&&hubHome.finish());
 clockMs+=5001;pump();assert(hubRelay.incomingBemfa("old001","off",false));old.command(command);assert(command["action"]=="outlet.set");old.result(command);assert(hubRelay.status=="connected");assert(hubRelay.incomingBemfa("new003","off",false));modern.command(command);assert(command["action"]=="fan.set"&&command["params"]["on"]==false);modern.result(command);assert(hubRelay.status=="connected");assert(!hubRelay.incomingBemfa("new003","on",true));
 modern.publish("lab/out",std::string("mp\0bytes",8));assert(upstream.size()==1&&upstream[0]==std::string("cloud/out:mp\0bytes",18));assert(!reports.empty());old.subscribe("lab/in");const std::string cloudPayload("cloud\0binary",12);hubRelay.incoming("cloud/in",cloudPayload,false);assert(old.payload("lab/in")==cloudPayload&&upstream.size()==1);
 auto route=config["hub"]["bridge"]["mappings"].as<JsonArray>().createNestedObject();route["direction"]="in";route["local_topic"]=modern.base+"/state";route["remote_topic"]="cloud/forged-state";auto beforeInjected=hubHome.devices[2].sequence;DynamicJsonDocument injected(512);injected["fan_on"]=false;injected["percentage"]=0;injected["led_visible"]=true;injected["brightness"]=128;hubRelay.incoming("cloud/forged-state",modern.encode(injected),false);assert(hubHome.devices[2].sequence==beforeInjected);
 modern.publish(modern.base+"/availability","offline",true);assert(!hubHome.devices[2].reachable(clockMs));assert(!hubHome.begin(2,iotcloud::HubHome::FanOn,0,1));modern.publish(modern.base+"/availability","online",true);modern.state(R"({"fan_on":false,"percentage":0,"led_visible":true,"brightness":128})");assert(hubHome.devices[2].reachable(clockMs));
 printf("PASS simulated application Bus JSON-TC1/MP-A1/MP-M1 discovery, HAP-model command+postACK state, cross-codec rejection, Bemfa roundtrip, cloud binary bridge both ways/injected-state isolation, offline recovery; reports=%zu upstream=%zu\n",reports.size(),upstream.size());
}
