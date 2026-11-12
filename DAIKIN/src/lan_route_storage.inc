// The loop stages an address only while committing an authenticated pair reply.
// The auth save callback runs under its mutex and commits config + keys in one
// blob. Unrelated incoming-pair writes cannot consume this staged route.
static const char *lanPairRoutePeer=nullptr,*lanPairRouteIP=nullptr;
static const ph_auth_pair_record *lanRouteRecord(const ph_auth_store &store,const char *peer){
  for(unsigned i=0;i<store.remote_count;i++)if(!strcmp(store.remote[i].peer,peer))return &store.remote[i];
  return nullptr;
}
static bool lanRouteSet(DynamicJsonDocument &candidate,const char *peer,const char *ip){
  JsonArray peers=candidate["lan_peers"].as<JsonArray>();
  if(peers.isNull())peers=candidate.createNestedArray("lan_peers");
  JsonObject target;for(JsonObject entry:peers)if(!strcmp(entry["peer"]|"",peer)){target=entry;break;}
  if(target.isNull()){
    if(peers.size()>=PH_AUTH_PAIR_SLOTS)return false;
    target=peers.createNestedObject();target["pair_created"]=true;
  }
  target["peer"]=String(peer);target["ip"]=String(ip);return !candidate.overflowed();
}
static bool lanRouteMatches(const char *peer,const char *ip){
  for(JsonObjectConst entry:config["lan_peers"].as<JsonArrayConst>())
    if(!strcmp(entry["peer"]|"",peer)&&!strcmp(entry["ip"]|"",ip))return true;
  return false;
}
static bool persistLanPairingStore(const ph_auth_store *store){
  bool added=false,removed=false;
  if(lanPairRoutePeer&&lanPairRouteIP){
    const auto *next=lanRouteRecord(*store,lanPairRoutePeer),*old=lanRouteRecord(localAuth.store,lanPairRoutePeer);
    added=next&&(!old||old->scopes!=next->scopes||memcmp(old->key,next->key,sizeof(next->key)));
  }
  for(JsonObjectConst entry:config["lan_peers"].as<JsonArrayConst>()){
    const char *peer=entry["peer"]|"";
    if(entry["pair_created"]==true&&lanRouteRecord(localAuth.store,peer)&&!lanRouteRecord(*store,peer)){removed=true;break;}
  }
  // Keep the existing allocation-free save path for unrelated auth changes.
  if(!added&&!removed&&config["_version"]==1)return persistAuthConfig(config,store);
  DynamicJsonDocument candidate(std::min(configCapacity,config.memoryUsage()+1024));
  if(!candidate.set(config))return false;
  JsonArray peers=candidate["lan_peers"].as<JsonArray>();
  // Explicitly imported/old fixed-IP pins have no pair_created marker.
  for(size_t i=peers.size();i>0;i--){
    JsonObject entry=peers[i-1];const char *peer=entry["peer"]|"";
    if(entry["pair_created"]==true&&lanRouteRecord(localAuth.store,peer)&&!lanRouteRecord(*store,peer))peers.remove(i-1);
  }
  if(removed&&!peers.size())candidate.remove("lan_peers");
  if(added&&!lanRouteSet(candidate,lanPairRoutePeer,lanPairRouteIP))return false;
  return persistAuthConfig(candidate,store);
}
