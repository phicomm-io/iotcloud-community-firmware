/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
namespace iotcloud {
// mDNS is an unauthenticated discovery hint. This registry never pairs a peer,
// registers an accessory, selects a legacy codec, or authorizes a command.
class HubLanDiscovery {
public:
  static constexpr size_t Capacity=12;
  struct Peer {
    std::string id,model,variant,firmware,ip;
    uint16_t port=0;uint32_t seen=0;
    bool online(uint32_t now)const{return !id.empty()&&uint32_t(now-seen)<90000;}
  };
  std::array<Peer,Capacity> peers;
  static bool ascii(const std::string &s,size_t maximum){if(s.empty()||s.size()>maximum)return false;for(unsigned char c:s)if(c<33||c>126)return false;return true;}
  static bool identity(const std::string &s){return s.size()==12&&s!="000000000000"&&s!="ffffffffffff"&&s.find_first_not_of("0123456789abcdef")==std::string::npos;}
  bool observe(const std::map<std::string,std::string> &txt,const std::string &ip,uint16_t port,uint32_t now){
    auto get=[&](const char *key)->std::string{auto it=txt.find(key);return it==txt.end()?"":it->second;};
    const auto id=get("id"),model=get("model"),variant=get("variant"),firmware=get("firmware");
    if(get("lan_wire")!="4"||get("codec")!="msgpack"||!identity(id)||
       (model!="a1"&&model!="m1"&&model!="tc1"&&model!="daikin")||!ascii(variant,32)||!ascii(firmware,64)||ip.empty()||!port)return false;
    Peer *slot=nullptr;
    for(auto &p:peers){if(p.id==id){slot=&p;break;}if((p.id.empty()||!p.online(now))&&!slot)slot=&p;}
    if(!slot)return false;
    slot->id=id;slot->model=model;slot->variant=variant;slot->firmware=firmware;slot->ip=ip;slot->port=port;slot->seen=now;return true;
  }
  const Peer *find(const std::string &id,uint32_t now)const{for(const auto &p:peers)if(p.id==id&&p.online(now))return &p;return nullptr;}
  void clear(){for(auto &p:peers)p=Peer();}
};
}
