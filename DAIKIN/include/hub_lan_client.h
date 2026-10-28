/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstring>
#include <functional>
#include <string>
#include <utility>
extern "C" {
#include "../../common/lan_wire_v4.h"
}
namespace iotcloud {
// One owner/one in-flight exchange. Writes retain their exact original bytes
// and transaction ID for bounded retries; no format fallback or new-ID replay.
class HubLanClient {
public:
  using Send=std::function<bool(const std::string &,uint16_t,const void *,size_t)>;
  using Complete=std::function<void(const std::string &,const void *,size_t)>;
  HubLanClient()=default;
  HubLanClient(const HubLanClient&)=delete;
  HubLanClient& operator=(const HubLanClient&)=delete;
  Send send;Complete complete;
  using Authorize=std::function<bool(const std::string &,const std::string &,uint16_t,const uint8_t *)>;
  Authorize authorize;
  const char *lastCode="idle";
  uint32_t lastId=0;
  bool busy()const{return pending.used;}
  ~HubLanClient(){reset();}
  void reset(){ph_buf_free(pending.response);pending=Pending();lastCode="cancelled";}
  bool begin(const char *local,uint32_t boot,const std::string &peer,const std::string &ip,uint16_t port,const uint8_t token[16],const std::string &route,const std::string &body,uint32_t now){
    if(busy()||!boot||!local||strlen(local)!=12||peer.size()!=12||ip.empty()||!port||body.empty()||body.size()>LAN_V4_REQUEST_MAX||lastId==UINT32_MAX)return false;
    if(!authorize||!authorize(peer,ip,port,token))return false;
    Pending next;next.used=true;next.local=local;next.boot=boot;next.peer=peer;next.ip=ip;next.port=port;next.route=route;next.body=body;next.id=++lastId;next.started=now;std::memcpy(next.token,token,16);
    pending=std::move(next);lastCode="pending";
    // A failed send is indeterminate: retain the exact admitted ID for retries.
    transmit(now);return true;
  }
  bool receive(const lan_v4_packet &packet,const std::string &ip,uint16_t port,uint32_t now){
    if(!busy()||packet.op!=LAN_V4_RESULT||ip!=pending.ip||port!=pending.port||packet.src!=pending.peer||packet.dst!=pending.local||packet.id!=pending.id||packet.request_boot!=pending.boot||std::memcmp(packet.token,pending.token,16))return false;
    if(!authorize||!authorize(pending.peer,pending.ip,pending.port,pending.token)){reset();lastCode="auth_required";return true;}
    if(std::strcmp(packet.code,"ok")){
      if(!std::strcmp(packet.code,"busy")){lastCode="busy";return true;}
      if(!std::strcmp(packet.code,"acked"))return true;
      // Terminal transport errors do not authorize another execution.
      const char *code=!std::strcmp(packet.code,"expired")?"expired":!std::strcmp(packet.code,"conflict")?"conflict":!std::strcmp(packet.code,"auth_required")?"auth_required":"transport_error";
      reset();lastCode=code;return true;
    }
    if(!packet.size||packet.size>LAN_V4_RESPONSE_MAX||packet.total!=(packet.size+LAN_V4_CHUNK-1)/LAN_V4_CHUNK||packet.part>=packet.total||!packet.body||packet.body_length!=(packet.part+1u==packet.total?packet.size-packet.part*LAN_V4_CHUNK:LAN_V4_CHUNK)){reset();lastCode="invalid_result";return true;}
    if(packet.part!=pending.nextPart)return true;
    if(!pending.response){
      if(packet.part)return true;
      pending.response=ph_buf_new(packet.size+1);
      if(!pending.response){reset();lastCode="busy";return true;}
      pending.size=packet.size;pending.total=packet.total;pending.deviceBoot=packet.boot;
    }
    if(packet.boot!=pending.deviceBoot||packet.size!=pending.size||packet.total!=pending.total||pending.response->len+packet.body_length>pending.size){reset();lastCode="conflict";return true;}
    std::memcpy(pending.response->buf+pending.response->len,packet.body,packet.body_length);pending.response->len+=packet.body_length;pending.response->buf[pending.response->len]=0;
    ++pending.nextPart;pending.attempts=0;
    if(pending.nextPart<pending.total){transmit(now);return true;}
    if(pending.response->len!=pending.size){reset();lastCode="conflict";return true;}
    ph_value root;if(!phv_parse(PH_WIRE_MSGPACK,pending.response->buf,pending.response->len,&root)||root.kind!=PHV_MAP){reset();lastCode="invalid_result";return true;}
    transmit(now,true);
    const std::string route=pending.route;ph_buf *response=pending.response;pending.response=nullptr;pending=Pending();lastCode="ok";
    // Completion may synchronously begin the next status query. The completed
    // owned buffer stays alive until that callback returns.
    if(complete)complete(route,response->buf,response->len);
    ph_buf_free(response);return true;
  }
  void tick(uint32_t now){
    if(!busy())return;
    if(!authorize||!authorize(pending.peer,pending.ip,pending.port,pending.token)){reset();lastCode="auth_required";return;}
    if(uint32_t(now-pending.started)>=LAN_V4_RESPONSE_MS){reset();lastCode="timeout";return;}
    if(uint32_t(now-pending.sent)>=400&&pending.attempts<4)transmit(now);
  }
private:
  struct Pending {
    bool used=false;std::string local,peer,ip,route,body;
    uint16_t port=0,nextPart=0,total=0,size=0;
    uint8_t token[16]={};uint32_t boot=0,deviceBoot=0,id=0,started=0,sent=0;
    unsigned attempts=0;ph_buf *response=nullptr;
  } pending;
  bool transmit(uint32_t now,bool ack=false){
    lan_v4_packet packet={};packet.op=ack?LAN_V4_ACK:pending.nextPart?LAN_V4_PULL:LAN_V4_COMMAND;
    std::memcpy(packet.src,pending.local.c_str(),13);std::memcpy(packet.dst,pending.peer.c_str(),13);std::memcpy(packet.token,pending.token,16);packet.boot=pending.boot;packet.id=pending.id;
    if(packet.op==LAN_V4_COMMAND){packet.body=pending.body.data();packet.body_length=pending.body.size();}else if(packet.op==LAN_V4_PULL)packet.part=pending.nextPart;
    char frame[LAN_V4_FRAME_MAX+1];ph_buf buffer;ph_writer writer;
    if(!phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,frame,sizeof(frame))||!lan_v4_encode(&writer,&packet))return false;
    const size_t length=phw_finish_fixed(&writer);pending.sent=now;++pending.attempts;
    return length&&send&&send(pending.ip,pending.port,frame,length);
  }
};
}
