/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <functional>
#include <string>
namespace iotcloud {
// Loop-owned, bounded waiting room for commands, separate from periodic reads.
// Borrowed MQTT/HTTP input is copied before admission. Never replay a command
// under a new transaction ID after the client has started it.
class HubLanQueue {
public:
  static constexpr unsigned Capacity=2;
  using Start=std::function<const char*(const std::string&,const std::string&)>;
  using Failed=std::function<void(const std::string&,const std::string&,const char*)>;
  bool push(const std::string &route,const std::string &body,uint32_t now){
    if(count==Capacity||body.empty()||body.size()>1024||route.empty()||route.size()>512)return false;
    auto &e=entries[(head+count)%Capacity];e.route=route;e.body=body;e.at=now;++count;return true;
  }
  unsigned size()const{return count;}
  void clear(){for(auto &e:entries)e=Entry();head=count=0;}
  void poll(uint32_t now,bool busy,const Start &start,const Failed &failed){
    if(!count)return;
    auto &e=entries[head];
    // A loop timestamp can precede admission in the same iteration.
    // Treat that as zero wait, not an unsigned 49-day timeout.
    const bool expired=int32_t(now-e.at)>=5000;
    const char *error=expired?"busy":busy?"busy":start(e.route,e.body);
    if(error&&!strcmp(error,"busy")&&!expired)return;
    // Remove before callbacks, which may synchronously queue another command.
    Entry done=std::move(e);head=(head+1)%Capacity;--count;
    if(error&&failed)failed(done.route,done.body,error);
  }
private:
  struct Entry {std::string route,body;uint32_t at=0;};
  std::array<Entry,Capacity> entries;
  unsigned head=0,count=0;
};
}
