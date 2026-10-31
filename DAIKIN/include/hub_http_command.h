/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <array>
#include <functional>
#include <string>
#include <cstring>
extern "C" {
#include "../../common/lan_wire_v4.h"
}
namespace iotcloud {
// One HTTP command owns its result independently of telemetry/MQTT receipts.
// Used on loop(); the server task receives the completed owned bytes only.
class HubHttpCommand {
public:
  using Guard=std::function<bool()>;
  static constexpr uint32_t Timeout=10500, ReceiptLifetime=120000;
  static constexpr size_t ReceiptCapacity=128;
  static bool parse(const std::string &body,std::string &id,bool *readOnly=nullptr) {
    ph_value root,action,params,request;char actionText[65]={},requestText[65]={};
    if(body.empty()||body.size()>LAN_V4_REQUEST_MAX||
       !phv_parse(PH_WIRE_MSGPACK,body.data(),body.size(),&root)||root.kind!=PHV_MAP||
       !phv_keys(&root,"|action||params||request_id|")||
       !phv_get(&root,"action",&action)||!phv_text(&action,actionText,sizeof(actionText))||!actionText[0]||strlen(actionText)!=action.size||
       !phv_get(&root,"params",&params)||params.kind!=PHV_MAP||
       !phv_get(&root,"request_id",&request)||!phv_text(&request,requestText,sizeof(requestText))||!requestText[0]||strlen(requestText)!=request.size)return false;
    id=requestText;
    if(readOnly)*readOnly=!strcmp(actionText,"status.get")||!strcmp(actionText,"capabilities.get")||
      !strcmp(actionText,"config.get")||!strcmp(actionText,"diagnostics.get")||
      !strcmp(actionText,"info.get")||!strcmp(actionText,"logs.get");
    return true;
  }
  const char *begin(const std::string &route,const std::string &body,Guard guard,uint32_t now) {
    if(active)return "busy";
    std::string id;bool readOnly=false;if(!parse(body,id,&readOnly))return "invalid_params";
    if(!guard||!guard())return "unauthorized";
    const uint64_t fingerprint=receiptKey(route,id);Receipt *available=nullptr;
    for(const auto &receipt:receipts)
      if(receipt.used&&uint32_t(now-receipt.at)<ReceiptLifetime&&receipt.key==fingerprint)
        return "conflict";
    if(!readOnly){
      for(auto &receipt:receipts)if(!receipt.used||uint32_t(now-receipt.at)>=ReceiptLifetime){available=&receipt;break;}
      // Never discard a still-protected write to admit another operation.
      if(!available)return "busy";
      available->used=true;available->key=fingerprint;available->at=now;
    }
    currentRoute=route;currentBody=body;currentId=std::move(id);valid=std::move(guard);started=now;
    response.clear();status=200;active=true;finished=false;return nullptr;
  }
  bool pending()const{return active&&!finished;}
  bool ready()const{return active&&finished;}
  bool matches(const std::string &route,const std::string &body)const {
    return pending()&&route==currentRoute&&body==currentBody;
  }
  bool expects(const std::string &route,const std::string &id)const {return pending()&&currentRoute==route&&currentId==id;}
  bool complete(const std::string &route,const void *bytes,size_t length) {
    if(!pending()||route!=currentRoute)return false;
    if(!valid||!valid()){fail("unauthorized");return true;}
    ph_value root,request,code;char id[65]={};
    if(!bytes||length>LAN_V4_RESPONSE_MAX||!phv_parse(PH_WIRE_MSGPACK,bytes,length,&root)||root.kind!=PHV_MAP||
       !phv_get(&root,"code",&code)||code.kind!=PHV_TEXT||
       !phv_get(&root,"request_id",&request)||!phv_text(&request,id,sizeof(id))||strlen(id)!=request.size||currentId!=id){fail("invalid_result");return true;}
    response.assign(static_cast<const char*>(bytes),length);finished=true;status=200;return true;
  }
  void failed(const std::string &route,const std::string &body,const char *reason) {
    if(matches(route,body))fail(reason);
  }
  void cancel(const char *reason){if(pending())fail(reason);}
  void tick(uint32_t now) {
    if(!pending())return;
    if(!valid||!valid())fail("unauthorized");
    else if(uint32_t(now-started)>=Timeout)fail("timeout");
  }
  bool take(std::string &bytes,int &httpStatus) {
    if(!ready())return false;
    // Logout/password changes may occur after the last datagram was processed.
    if(!valid||!valid())fail("unauthorized");
    bytes=std::move(response);httpStatus=status;active=finished=false;
    currentRoute.clear();currentBody.clear();currentId.clear();valid=Guard();return true;
  }
private:
  struct Receipt {uint64_t key=0;uint32_t at=0;bool used=false;};
  std::array<Receipt,ReceiptCapacity> receipts;
  static uint64_t receiptKey(const std::string &route,const std::string &id){
    // A collision only refuses an otherwise new operation. Identical writes
    // always have the same key, so this compact index cannot permit a replay.
    uint64_t key=UINT64_C(14695981039346656037);
    for(unsigned char c:route){key^=c;key*=UINT64_C(1099511628211);}
    key^=0;key*=UINT64_C(1099511628211);
    for(unsigned char c:id){key^=c;key*=UINT64_C(1099511628211);}
    return key;
  }
  std::string currentRoute,currentBody,currentId,response;Guard valid;
  uint32_t started=0;int status=200;bool active=false,finished=false;
  void fail(const char *reason) {
    const char *code=!strcmp(reason,"busy")?"busy":!strcmp(reason,"unauthorized")?"unauthorized":
      !strcmp(reason,"auth_required")?"auth_required":!strcmp(reason,"invalid_params")?"invalid_params":
      !strcmp(reason,"offline")?"offline":!strcmp(reason,"not_registered")?"not_registered":
      !strcmp(reason,"route_changed")?"route_changed":!strcmp(reason,"conflict")?"conflict":"transport_error";
    status=!strcmp(code,"unauthorized")?401:!strcmp(code,"invalid_params")?400:
      !strcmp(code,"auth_required")?403:!strcmp(code,"conflict")?409:503;
    char storage[320];ph_buf buffer;ph_writer writer;
    response.clear();
    if(phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,storage,sizeof(storage))){
      phw_begin(&writer,nullptr,false);phw_text(&writer,"code",code);phw_text(&writer,"request_id",currentId.c_str());
      phw_begin(&writer,"data",false);phw_text(&writer,"reason",reason);phw_end(&writer);phw_end(&writer);
      const size_t length=phw_finish_fixed(&writer);if(length)response.assign(storage,length);
    }
    finished=true;
  }
};
}
