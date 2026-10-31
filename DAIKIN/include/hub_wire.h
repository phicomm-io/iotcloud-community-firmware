/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <ArduinoJson.h>
#include <array>
#include <string>
namespace iotcloud {
// Legacy codec selection uses discovery; S30-V1B's product contract is binary
// only. Neither path changes codec in response to a failed write.
class HubWire {
  struct Entry {std::string base;bool binary=false;};
  std::array<Entry,17> devices;
public:
  void clear(){for(auto &d:devices)d=Entry();}
  bool binary(const std::string &base)const {
    const auto at=base.rfind('/');
    if(at!=std::string::npos&&at>=8&&base.compare(at-8,8,"/s30-v1b")==0)return true;
    for(const auto &d:devices)if(d.base==base)return d.binary;
    return false;
  }
  static bool printable(JsonVariantConst value,size_t maximum){if(!value.is<const char*>())return false;auto text=value.as<std::string>();if(text.empty()||text.size()>maximum)return false;for(unsigned char c:text)if(c<32||c>126)return false;return true;}
  void observe(const std::string &topic,const std::string &body){
    auto at=topic.find("/discovery/");if(at==std::string::npos||body.size()>1024)return;
    DynamicJsonDocument doc(2048);
    bool mp=!body.empty()&&((uint8_t(body[0])&0xf0)==0x80||uint8_t(body[0])==0xde);
    auto error=mp?deserializeMsgPack(doc,body):deserializeJson(doc,body);
    if(error||!doc.is<JsonObject>()||doc["protocol"]!="tmcp"||doc["protocol_version"]!=2)return;
    std::string mac=doc["device_id"]|topic.substr(at+11).c_str(),model=doc["model"]|"";
    if(mac.size()!=12||mac=="000000000000"||mac=="ffffffffffff"||mac.find_first_not_of("0123456789abcdef")!=std::string::npos||mac!=topic.substr(at+11)||(model!="a1"&&model!="m1"&&model!="tc1"&&model!="daikin"&&model!="s30-v1b"))return;
    bool binary=mp&&doc["schema"]==2&&doc["version"]=="v2"&&printable(doc["firmware_version"],64)&&printable(doc["variant"],32)&&doc["wire_version"]==1&&doc["default_wire_format"]=="msgpack"&&doc["wire_formats"].is<JsonArray>()&&doc["wire_formats"].size()==1&&doc["wire_formats"][0]=="msgpack";
    if(!binary&&(mp||doc["schema"]!=1||model=="s30-v1b"))return;
    std::string base=topic.substr(0,at+1)+model+"/"+mac;
    Entry *slot=nullptr;for(auto &d:devices){if(d.base==base){slot=&d;break;}if(d.base.empty()&&!slot)slot=&d;}
    if(slot){slot->base=base;slot->binary=binary;}
  }
  DeserializationError decode(JsonDocument &doc,const std::string &base,const std::string &body)const{auto error=binary(base)?deserializeMsgPack(doc,body):deserializeJson(doc,body);return error?error:doc.is<JsonObject>()?DeserializationError::Ok:DeserializationError::InvalidInput;}
  void encode(const JsonDocument &doc,const std::string &base,std::string &body)const{if(binary(base))serializeMsgPack(doc,body);else serializeJson(doc,body);}
};
static HubWire hubWire;
}
