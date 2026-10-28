/* SPDX-License-Identifier: GPL-3.0-or-later */
// All inventory, UDP transaction and business routing state belongs to loop().
// mDNS only supplies explicit capability/routing hints, never credentials.
static iotcloud::HubLanDiscovery hubLanDiscovery;
static bool hubLanRouteConfirmed(const iotcloud::HubLanDiscovery::Peer &);
static mdns_search_once_t *hubLanSearch=nullptr;
static uint32_t hubLanNextDiscovery=0;
static bool hubLanRegistered(const iotcloud::HubLanDiscovery::Peer &peer){
  for(JsonObjectConst d:config["hub"]["devices"].as<JsonArrayConst>())
    if(d["mac"]==peer.id && d["model"]==peer.model && d["status"]=="active")return true;
  return false;
}
static bool hubLanPaired(const std::string &id){
  char key[33]={};unsigned scopes=0;
  const bool found=localAuthReady&&ph_auth_remote_get(&localAuth,id.c_str(),key,&scopes);
  memset(key,0,sizeof(key));return found&&(scopes&PH_AUTH_SCOPE_CONTROL);
}
static void hubLanStatus(JsonObject out){
  const uint32_t now=millis();out["ready"]=hubReady();
  JsonArray rows=out["devices"].as<JsonArray>();if(rows.isNull())rows=out.createNestedArray("devices");
  for(const auto &peer:hubLanDiscovery.peers){
    if(peer.id.empty())continue;JsonObject row=rows.createNestedObject();
    row["device_id"]=peer.id;row["model"]=peer.model;row["variant"]=peer.variant;row["firmware"]=peer.firmware;
    row["ip"]=peer.ip;row["port"]=peer.port;row["compatible"]=true;
    row["registered"]=hubLanRegistered(peer);row["paired"]=hubLanPaired(peer.id);row["online"]=peer.online(now);
    row["code"]=!peer.online(now)?"offline":!hubLanRegistered(peer)?"not_registered":!hubLanPaired(peer.id)?"auth_required":!hubLanRouteConfirmed(peer)?"route_changed":"ready";
  }
}
static void hubLanDiscover(uint32_t now){
  if(hubLanSearch){
    mdns_result_t *results=nullptr;
    if(!mdns_query_async_get_results(hubLanSearch,0,&results))return;
    mdns_query_async_delete(hubLanSearch);hubLanSearch=nullptr;
    for(auto *result=results;result;result=result->next){
      if(!result->service_type||strcmp(result->service_type,LAN_V4_MDNS_SERVICE)||!result->proto||strcmp(result->proto,LAN_V4_MDNS_PROTO))continue;
      std::map<std::string,std::string> txt;bool duplicate=false;
      for(size_t i=0;i<result->txt_count;i++){
        const char *key=result->txt[i].key,*value=result->txt[i].value;
        if(!key||!value||strlen(key)>32||result->txt_value_len[i]>64||!txt.emplace(key,std::string(value,result->txt_value_len[i])).second){duplicate=true;break;}
      }
      if(duplicate||txt["id"]==deviceId.c_str())continue;
      for(auto *address=result->addr;address;address=address->next){
        if(address->addr.type!=IPADDR_TYPE_V4)continue;
        const String ip=IPAddress(address->addr.u_addr.ip4.addr).toString();
        if(hubLanDiscovery.observe(txt,ip.c_str(),result->port,now))break;
      }
    }
    mdns_query_results_free(results);
  }
  if(WiFi.status()!=WL_CONNECTED||!due(now,hubLanNextDiscovery))return;
  // Async IDF API: never block local buttons/HAP for a synchronous PTR query.
  hubLanNextDiscovery=now+30000;
  hubLanSearch=mdns_query_async_new(nullptr,LAN_V4_MDNS_SERVICE,LAN_V4_MDNS_PROTO,MDNS_TYPE_PTR,1800,iotcloud::HubLanDiscovery::Capacity,nullptr);
}
static iotcloud::HubLanClient hubLanClient;
static uint32_t hubLanBoot=0;
static bool hubLanRequestStatus=false;
static std::string hubLanRequestId;
static uint32_t hubLanNextStatus[iotcloud::HubLanDiscovery::Capacity]={};
static bool hubLanRouteConfirmed(const iotcloud::HubLanDiscovery::Peer &peer){
  for(JsonObjectConst entry:config["lan_peers"].as<JsonArrayConst>())
    if(entry["peer"]==peer.id&&entry["ip"]==peer.ip)return true;
  return false;
}
static void hubLanReset(){hubLanClient.reset();}
static void hubLanObserveDiscovery(const iotcloud::HubLanDiscovery::Peer &peer){
  DynamicJsonDocument doc(1024);doc["schema"]=2;doc["model"]=peer.model;doc["protocol"]="tmcp";doc["protocol_version"]=2;doc["version"]="v2";
  doc["device_id"]=peer.id;doc["firmware_version"]=peer.firmware;doc["variant"]=peer.variant;doc["wire_version"]=1;doc["default_wire_format"]="msgpack";doc.createNestedArray("wire_formats").add("msgpack");
  std::string bytes;serializeMsgPack(doc,bytes);iotcloud::hubWire.observe(hubService.prefix()+"discovery/"+peer.id,bytes);
}
static bool hubLanSend(const std::string &route,const std::string &body,bool retain){
  if(retain||!hubReady()||otaActive||hubLanClient.busy()||body.size()>LAN_V4_REQUEST_MAX||!hubAllocationReady(body.size()+LAN_V4_RESPONSE_MAX+4096))return false;
  const uint32_t now=millis();
  for(const auto &peer:hubLanDiscovery.peers){
    if(!peer.online(now)||route!=hubService.prefix()+peer.model+"/"+peer.id+"/command")continue;
    if(!hubLanRegistered(peer)||!hubLanPaired(peer.id)||!hubLanRouteConfirmed(peer))return false;
    ph_value root,action,params;
    if(!phv_parse(PH_WIRE_MSGPACK,body.data(),body.size(),&root)||root.kind!=PHV_MAP||!phv_keys(&root,"|action||params||request_id|")||!phv_get(&root,"action",&action)||action.kind!=PHV_TEXT||!phv_get(&root,"params",&params)||params.kind!=PHV_MAP)return false;
    initLanClient();uint8_t token[16]={};
    const bool authorized=lc_token(&lanClient,&lanOps,peer.id.c_str(),peer.ip.c_str(),PH_AUTH_SCOPE_CONTROL,token);
    if(!authorized){memset(token,0,sizeof(token));return false;}
    hubLanObserveDiscovery(peer);
    ph_value requestId;char id[65]={};
    if(phv_get(&root,"request_id",&requestId)&&!phv_text(&requestId,id,sizeof(id))){memset(token,0,sizeof(token));return false;}
    const bool status=phv_equal_text(&action,"status.get");
    const bool started=hubLanClient.begin(deviceId.c_str(),hubLanBoot,peer.id,peer.ip,peer.port,token,route,body,now);
    if(started){hubLanRequestStatus=status;hubLanRequestId=id;}
    memset(token,0,sizeof(token));return started;
  }
  return false;
}
static bool hubLanPacket(const ph_value *root,const char *ip,uint16_t port){
  ph_value op;if(!phv_get(root,"op",&op)||!phv_equal_text(&op,"result"))return false;
  ph_value version;int64_t wire;if(!phv_get(root,"v",&version)||!phv_int(&version,&wire)||wire!=4)return false;
  lan_v4_packet packet={};if(!lan_v4_decode_value(root,&packet))return true;
  hubLanClient.receive(packet,ip,port,millis());return true;
}
static void initHubLan(){
  hubLanBoot=sensorBoot;
  hubLanClient.authorize=[](const std::string &id,const std::string &ip,uint16_t port,const uint8_t *token){
    const auto *peer=hubLanDiscovery.find(id,millis());
    if(!peer||peer->ip!=ip||peer->port!=port||!hubLanRegistered(*peer)||!hubLanPaired(id)||!hubLanRouteConfirmed(*peer))return false;
    lc_job *job=lc_find(&lanClient,id.c_str(),false);
    return job&&job->phase==LC_READY&&job->epoch==ph_auth_credential_epoch(&localAuth)&&(job->scopes&PH_AUTH_SCOPE_CONTROL)&&ip==job->ip&&!memcmp(job->token,token,16);
  };
  hubLanClient.send=[](const std::string &ip,uint16_t port,const void *bytes,size_t length){
    IPAddress address;if(!address.fromString(ip.c_str())||!sensorUdp.beginPacket(address,port))return false;
    return sensorUdp.write(static_cast<const uint8_t*>(bytes),length)==length&&sensorUdp.endPacket()==1;
  };
  hubLanClient.complete=[](const std::string &command,const void *bytes,size_t length){
    if(!hubReady()||command.size()<8)return;
    const bool status=hubLanRequestStatus;const std::string expectedId=hubLanRequestId;
    iotcloud::routeHubLanResult(hubDeviceBus,command,bytes,length,status,expectedId);
  };
  hubService.lanOperation=[](const std::string &op,JsonObjectConst input,JsonObject output)->const char*{
    if(op=="status")return lanPairStatus(input["peer"]|"",output)?"ok":"invalid_params";
    const char *code="invalid_params";
    if(op=="pair")lanPairStart(input["peer"]|"",input["ip"]|"",input["password"]|"",input["scopes"]|0,&code);
    else if(op=="unpair")lanUnpairStart(input["peer"]|"",&code);
    return code;
  };
}
static void pollHubLan(uint32_t now){
  if(hubRestartPending()){hubLanReset();return;}
  if(otaActive){hubLanReset();return;}
  // Also called inside HomeKit's bounded wait so UDP receipts can complete.
  pollSessions();pollLanPairing();hubLanClient.tick(now);hubLanDiscover(now);
  if(!hubReady()||hubLanClient.busy())return;
  for(size_t i=0;i<hubLanDiscovery.peers.size();i++){
    const auto &peer=hubLanDiscovery.peers[i];
    if(!peer.online(now)||!due(now,hubLanNextStatus[i])||!hubLanRegistered(peer)||!hubLanPaired(peer.id)||!hubLanRouteConfirmed(peer))continue;
    hubLanNextStatus[i]=now+5000;
    // Refresh even when HomeKit is disabled: relay/cloud consumers need live
    // measured state independently of whether a HAP accessory is enabled.
    DynamicJsonDocument request(384);request["action"]="status.get";request.createNestedObject("params");
    request["request_id"]=std::string("lan-")+std::to_string(hubLanBoot)+"-"+std::to_string(hubLanClient.lastId+1);
    std::string body;serializeMsgPack(request,body);hubLanSend(hubService.prefix()+peer.model+"/"+peer.id+"/command",body,false);break;
  }
}
