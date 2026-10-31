// Production HubHome with real MessagePack. No Home controller or DUT fixture.
#include "hub_home.h"
#include <cassert>
#include <cstdio>
using Home=iotcloud::HubHome;
static std::string wire(const JsonDocument &doc){std::string out;serializeMsgPack(doc,out);return out;}
static void discovery(const Home::Device &d){
  DynamicJsonDocument doc(1024);doc["schema"]=2;doc["protocol"]="tmcp";doc["protocol_version"]=2;
  doc["version"]="v2";doc["device_id"]=d.mac;doc["model"]=d.model;doc["variant"]="full";
  doc["firmware_version"]="test";doc["wire_version"]=1;doc["default_wire_format"]="msgpack";
  doc.createNestedArray("wire_formats").add("msgpack");
  iotcloud::hubWire.observe("lab/discovery/"+d.mac,wire(doc));
}
static void state(Home &home,size_t i,const std::string &name){
  const auto &d=home.devices[i];DynamicJsonDocument doc(2048);
  doc["name"]=name;doc["model"]=d.model;doc["device_id"]=d.mac;
  if(d.outletCount()){
    auto states=doc.createNestedArray("socket_states");
    for(unsigned n=0;n<d.outletCount();++n){if(d.model=="tc1")states.add(0);else states.add(false);}
  }else if(d.model=="a1"){
    doc["fan_on"]=false;doc["percentage"]=0;doc["led_visible"]=false;doc["brightness"]=0;
  }else if(d.model=="m1")doc["sensor_valid"]=false;
  else doc.createNestedObject("sensors");
  home.observe(d.base+"/state",wire(doc));
}
int main(){
  DynamicJsonDocument saved(1024);auto a=saved.to<JsonArray>();
  unsigned id=1;
  for(const char *model:{"tc1","m1","a1","s30-v1b"}){
    auto d=a.createNestedObject();char mac[13];snprintf(mac,sizeof(mac),"02c4000000%02x",id++);
    d["model"]=model;d["mac"]=mac;
  }
  iotcloud::HubDeviceBus bus;Home home(bus);
  assert(home.configure(saved.as<JsonArrayConst>(),"lab/","02c400000099"));
  unsigned serial=0;home.randomHex=[&](size_t n){auto s=std::to_string(++serial);return std::string(2*n-s.size(),'0')+s;};
  std::string route,body;unsigned writes=0;bool admit=true;
  bus.dispatch=[&](const std::string &r,const std::string &b,bool retain,iotcloud::HubDeviceBus::Origin origin){
    assert(!retain&&origin==iotcloud::HubDeviceBus::Origin::Internal);route=r;body=b;
    DynamicJsonDocument doc(1024);assert(!deserializeMsgPack(doc,b));
    if(doc["action"]=="name.set")++writes;
    return admit;
  };
  uint32_t now=10001;home.tick(now);
  for(size_t i=0;i<home.size;++i){discovery(home.devices[i]);state(home,i,"original");}
  assert(!home.beginName(1,"unadvertised"));
  auto capabilities=[&](size_t i,unsigned limit,bool supported=true,const char *identity=nullptr){
    home.refresh();DynamicJsonDocument query(1024);assert(!deserializeMsgPack(query,body));
    assert(route==home.devices[i].base+"/command"&&query["action"]=="capabilities.get");
    DynamicJsonDocument reply(1024);reply["request_id"]=query["request_id"];reply["code"]="ok";
    auto data=reply.createNestedObject("data");data["device_id"]=identity?identity:home.devices[i].mac.c_str();
    data["model"]=home.devices[i].model;data["name_max_bytes"]=limit;
    auto actions=data.createNestedArray("actions");actions.add("status.get");if(supported)actions.add("name.set");
    home.observe(home.devices[i].base+"/result",wire(reply));
  };
  for(size_t i=0;i<home.size;++i)capabilities(i,i==1?31:63);
  for(size_t i=0;i<home.size;++i){
    const auto aid=home.devices[i].aid;const unsigned limit=home.devices[i].nameMax;
    assert(limit==(i==1?31u:63u));
    for(const auto &bad:{std::string(),std::string(limit+1,'x'),std::string("a\0b",3),
                       std::string("\xed\xa0\x80"),std::string("\xc0\xaf"),std::string("\xf4\x90\x80\x80")})
      assert(!home.beginName(i,bad));
    const std::string name=i==1?std::string(31,'t'):std::string("卧室, M1: [温度] \"name\" \\ 😀");
    assert(home.beginName(i,name));
    DynamicJsonDocument cmd(1024);assert(!deserializeMsgPack(cmd,body));
    assert(cmd["action"]=="name.set"&&cmd["params"].size()==1&&cmd["params"]["name"]==name);
    assert(!home.beginName(i,"second"));
    DynamicJsonDocument ack(512);ack["request_id"]=cmd["request_id"];ack["code"]="accepted";
    // Pre-receipt state and unrelated receipts cannot confirm a write.
    state(home,i,name);DynamicJsonDocument unrelated(256);unrelated["code"]="ok";unrelated["request_id"]="other";
    home.observe(home.devices[i].base+"/result",wire(unrelated));
    assert(!home.finished());home.observe(home.devices[i].base+"/result",wire(ack));assert(!home.finished());
    assert(!deserializeMsgPack(cmd,body)&&cmd["action"]=="status.get");
    state(home,i,name);assert(home.finished()&&home.finish());
    assert(home.devices[i].name==name&&home.devices[i].aid==aid);
  }
  // An error is terminal and cannot install a desired-only local alias.
  assert(home.beginName(2,"unsaved"));DynamicJsonDocument cmd(1024),ack(512);assert(!deserializeMsgPack(cmd,body));
  const auto old=home.devices[2].name;
  ack["request_id"]=cmd["request_id"];ack["code"]="storage_error";
  home.observe(home.devices[2].base+"/result",wire(ack));assert(home.finished()&&!home.finish()&&home.devices[2].name==old);
  assert(home.beginName(2,"timeout"));assert(!deserializeMsgPack(cmd,body));
  ack["request_id"]=cmd["request_id"];ack["code"]="accepted";
  assert(!home.finish());home.observe(home.devices[2].base+"/result",wire(ack));
  assert(!home.finished()&&home.devices[2].name==old); // Late ACK never replays.
  home.observe(home.devices[2].base+"/availability","offline");assert(!home.beginName(2,"offline"));
  home.observe(home.devices[2].base+"/availability","online");state(home,2,old);
  admit=false;assert(!home.beginName(2,"busy"));admit=true;assert(!home.finished());
  // Two devices may have the same name; identity remains the original MAC AID.
  state(home,2,"duplicate");state(home,3,"duplicate");assert(home.devices[2].aid!=home.devices[3].aid);
  now+=60000;home.tick(now);for(size_t i=0;i<home.size;++i)state(home,i,"original");
  capabilities(0,63,false);capabilities(1,63);capabilities(2,63,true,"02c4000000ff");
  assert(!home.beginName(0,"old firmware")&&!home.beginName(1,"invalid TC1 bound")&&!home.beginName(2,"wrong device"));
  now+=120000;home.tick(now);state(home,3,"current");assert(!home.beginName(3,"stale capability"));
  assert(writes==8); // Five confirmed + failed + timed-out + rejected admission.
  puts("HomeKit names: MP capabilities, byte bounds, exact ACK/state confirmation, failure/late ACK, old firmware and stable identity passed");
}
