// Production pairing uses the same borrowed MP parser and auth state as MiCO.
static WiFiUDP sensorUdp;
#define AUTH_DEVICE_ID deviceId.c_str()
#include "../../common/lan_auth.inc"
#undef AUTH_DEVICE_ID
#include "../../common/lan_client_core.inc"
static lc_state lanClient={};
static lc_ops lanOps={};
static const unsigned LC_UNPAIR=5;
static bool lanSend(const char *ip,const void *data,size_t length){IPAddress address;if(!address.fromString(ip)||!sensorUdp.beginPacket(address,11288))return false;return sensorUdp.write(static_cast<const uint8_t*>(data),length)==length&&sensorUdp.endPacket()==1;}
static void initLanClient(){
  if(lanOps.local)return;
  lanOps.local=deviceId.c_str();lanOps.now=[]()->uint32_t{return millis();};lanOps.epoch=[](){return ph_auth_credential_epoch(&localAuth);};
  lanOps.random=[](uint8_t nonce[16]){esp_fill_random(nonce,16);return true;};
  lanOps.load=[](const char *peer,char key[33],unsigned *scopes){return localAuthReady&&ph_auth_remote_get(&localAuth,peer,key,scopes);};
  lanOps.save=[](const char *peer,const char *key,unsigned scopes,uint32_t epoch){
    lc_job *job=lc_find(&lanClient,peer,false);if(!localAuthReady||!job||job->phase!=LC_PAIR)return false;
    lanPairRoutePeer=peer;lanPairRouteIP=job->ip;
    const bool saved=ph_auth_remote_store(&localAuth,peer,key,scopes,epoch);
    lanPairRoutePeer=nullptr;lanPairRouteIP=nullptr;
    if(!saved)return false;
    // Re-pairing an unchanged key is a core no-op. The owner may still have
    // explicitly selected a new IP; commit that pin without rotating the key.
    if(lanRouteMatches(peer,job->ip))return true;
    DynamicJsonDocument candidate(std::min(configCapacity,config.memoryUsage()+1024));
    return candidate.set(config)&&lanRouteSet(candidate,peer,job->ip)&&saveConfig(candidate);
  };
  lanOps.send=lanSend;
#ifdef IOTCLOUD_HUB
  lanOps.business_boot=[]()->uint32_t{return sensorBoot;};
#endif
}
static bool lanPairStart(const char *peer,const char *ip,const char *password,unsigned scopes,const char **code){
  initLanClient();if(configRecovery||!localAuthReady){*code="migration_required";return false;}
  // The selected address belongs to this pending job until authenticated pair
  // success commits its route and key together. Rejections do not pin an IP.
  IPAddress address;
  if(!ip||!address.fromString(ip)||!lc_peer(peer)||!strcmp(peer,deviceId.c_str())||!password||strlen(password)>32||!scopes||(scopes&~7u)){*code="invalid_params";return false;}
  lc_job *existing=lc_find(&lanClient,peer,false);if(existing&&(existing->phase==LC_PAIR||existing->phase==LC_RESUME||existing->phase==LC_UNPAIR)){*code="busy";return false;}
  if(config["lan_peers"].size()>=LC_SLOTS){
    bool pinned=false;for(JsonObjectConst entry:config["lan_peers"].as<JsonArrayConst>())if(!strcmp(entry["peer"]|"",peer)){pinned=true;break;}
    if(!pinned){*code="busy";return false;}
  }
  return lc_start(&lanClient,&lanOps,peer,ip,password,scopes,code);
}
static bool lanUnpairStart(const char *peer,const char **code){
  initLanClient();lc_job *job=lc_find(&lanClient,peer,false);*code="auth_required";
  if(!job||job->phase!=LC_READY||job->epoch!=lanOps.epoch())return false;
  if(!lanOps.random(job->nonce)){*code="busy";return false;}
  job->phase=LC_UNPAIR;job->attempts=0;job->sent=0;job->code="pending";*code="accepted";return true;
}
static bool lanUnpairReply(const ph_value *root,const char *ip){
  ph_value value;char op[20],peer[13],code[32];const void *nonce;size_t n;
  if(!phv_get(root,"op",&value)||!phv_text(&value,op,sizeof(op))||strcmp(op,"auth_unpair")||!phv_get(root,"code",&value))return false;
  if(!phv_text(&value,code,sizeof(code))||!phv_keys(root,"|op||code||src||nonce|")||!phv_get(root,"src",&value)||!phv_text(&value,peer,sizeof(peer))||!phv_get(root,"nonce",&value)||!phv_bytes(&value,&nonce,&n)||n!=16)return true;
  lc_job *job=lc_find(&lanClient,peer,false);if(!job||job->phase!=LC_UNPAIR||strcmp(ip,job->ip)||memcmp(nonce,job->nonce,16)||job->epoch!=lanOps.epoch())return true;
  if(strcmp(code,"ok")){lc_fail(job,"auth_required");return true;}
  // Only an authenticated peer acknowledgement justifies remote revocation.
  bool removed=ph_auth_remote_remove(&localAuth,peer);lc_fail(job,removed?"unpaired":"storage_error");return true;
}
static bool lanPairStatus(const char *peer,JsonObject out){
  initLanClient();if(!lc_peer(peer))return false;lc_job *job=lc_find(&lanClient,peer,false);
  out["phase"]=!job?"idle":job->phase==LC_READY?"ready":job->phase==LC_FAILED?"failed":job->phase==LC_PAIR?"pairing":job->phase==LC_UNPAIR?"unpairing":"resuming";
  out["code"]=job&&job->code?job->code:"not_paired";return true;
}
static void pollLanPairing(){
  if(!localAuthReady||configRecovery)return;initLanClient();
  for(JsonObjectConst entry:config["lan_peers"].as<JsonArrayConst>()){
    const char *peer=entry["peer"]|"",*ip=entry["ip"]|"";if(!lc_peer(peer)||strlen(ip)>15)continue;
    lc_job *job=lc_find(&lanClient,peer,false);
    if(job&&(job->phase==LC_PAIR||job->phase==LC_RESUME||job->phase==LC_UNPAIR||
      (job->phase==LC_FAILED&&uint32_t(millis()-job->sent)<60000u)))continue;
    char key[33];unsigned scopes=0;if(!lanOps.load(peer,key,&scopes))continue;memset(key,0,sizeof(key));
    unsigned scope=(scopes&PH_AUTH_SCOPE_SENSOR)?PH_AUTH_SCOPE_SENSOR:(scopes&PH_AUTH_SCOPE_CONTROL)?PH_AUTH_SCOPE_CONTROL:PH_AUTH_SCOPE_GROUP;
    uint8_t token[16];lc_token(&lanClient,&lanOps,peer,ip,scope,token);memset(token,0,sizeof(token));
  }
  lc_tick(&lanClient,&lanOps);
  for(auto &job:lanClient.jobs)if(job.phase==LC_UNPAIR){
    if(job.epoch!=lanOps.epoch()){lc_fail(&job,"auth_required");continue;}
    if(job.attempts&&uint32_t(millis()-job.sent)<1000)continue;
    if(job.attempts>=3){lc_fail(&job,"timeout");continue;}
    char bytes[256];ph_buf buffer;ph_writer writer;phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,bytes,sizeof(bytes));
    phw_begin(&writer,nullptr,false);phw_text(&writer,"op","auth_unpair");phw_text(&writer,"src",deviceId.c_str());phw_bytes_value(&writer,"nonce",job.nonce,16);phw_bytes_value(&writer,"token",job.token,16);phw_end(&writer);
    size_t n=phw_finish_fixed(&writer);job.sent=millis();job.attempts++;if(n)lanSend(job.ip,bytes,n);
  }
}
static bool lanPairPacket(const ph_value *root,const char *ip){
  if(configRecovery||!localAuthReady)return true;
  initLanClient();if(lanUnpairReply(root,ip))return true;if(lc_response(&lanClient,&lanOps,root,ip))return true;
  char bytes[384];ph_buf buffer;ph_writer writer;if(!phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,bytes,sizeof(bytes)))return true;
  if(!feature_lan_pair(root,&writer,ip))return false;size_t length=phw_finish_fixed(&writer);
  if(length)lanSend(ip,bytes,length);memset(bytes,0,sizeof(bytes));return true;
}
