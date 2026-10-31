/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hub_wire.h"
#include <cassert>
#include <iostream>
#include <vector>

using iotcloud::HubWire;
static const std::string prefix="phicomm/v2/u_61646d696e/";
static const std::string mac="112233445566",base=prefix+"m1/"+mac;
static std::string pack(const JsonDocument &doc){std::string out;serializeMsgPack(doc,out);return out;}
static std::string json(const JsonDocument &doc){std::string out;serializeJson(doc,out);return out;}

// The previous StringReader overload is the compatibility oracle. Both paths
// copy const-input strings into the same bounded document and leave the input
// unchanged, including embedded NULs and bytes beyond a complete root value.
static void same(HubWire &wire,bool binary,const std::string &body){
  DynamicJsonDocument old(2048),current(2048);
  const auto before=body;
  auto expected=binary?deserializeMsgPack(old,body):deserializeJson(old,body);
  if(!expected&&!old.is<JsonObject>())expected=DeserializationError::InvalidInput;
  const auto actual=wire.decode(current,base,body);
  assert(actual==expected&&body==before);
  if(!actual)assert(json(current)==json(old));
}
int main(){
  HubWire wire;
  const std::string plain=R"({"name":"客厅,\"A\":[]","number":1.25,"active":true})";
  std::vector<std::string> legacy={"",plain,plain+" ",plain+"trailing",plain+"{}",
      std::string("{}\0extra",8),std::string("{\"name\":\"a\0b\"}",14),
      R"({"name":"a\u0000b"})",R"({"name":"\ud83d\ude00"})",
      "{\"number\":", "{\"name\":\"unterminated", "[]", "null", "{\"n\":NaN}"};
  for(const auto &body:legacy)same(wire,false,body);
  DynamicJsonDocument discovery(2048);
  assert(!deserializeJson(discovery,R"({"schema":2,"protocol":"tmcp","protocol_version":2,"version":"v2","variant":"standard","firmware_version":"1.0.0-rc.5","wire_version":1,"default_wire_format":"msgpack","wire_formats":["msgpack"]})"));
  discovery["device_id"]=mac;discovery["model"]="m1";
  const auto advert=pack(discovery);
  wire.observe(prefix+"discovery/"+mac,advert);
  assert(wire.binary(base));
  DynamicJsonDocument source(1024);
  source["name"]=std::string("a\0b",3);source["number"]=1.25;source["active"]=true;
  const auto binary=pack(source);
  DynamicJsonDocument decoded(2048);assert(!wire.decode(decoded,base,binary));
  assert(decoded["name"].as<JsonString>().size()==3);
  const std::vector<std::string> messages={"",binary,binary+"trailing",binary+std::string("\0",1),
      binary.substr(0,binary.size()-1),std::string("\x81\xa4name\xd9\x05" "ab",10),
      std::string("\xc0",1),std::string("\x90",1),std::string("\xc1",1)};
  for(const auto &body:messages)same(wire,true,body);
  // The discovery input uses the same bounded const reader. A truncated or
  // embedded-NUL string must not advertise binary support for a new peer.
  HubWire rejected;
  rejected.observe(prefix+"discovery/"+mac,advert.substr(0,advert.size()-1));
  assert(!rejected.binary(base));
  discovery["device_id"]=std::string("112233\0" "445566",13);
  rejected.observe(prefix+"discovery/"+mac,pack(discovery));
  assert(!rejected.binary(base));
  std::cout<<"Production bounded HubWire readers preserve legacy JSON/MessagePack NUL, trailing, truncation and discovery semantics\n";
}
