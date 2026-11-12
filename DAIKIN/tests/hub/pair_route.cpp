#define main auth_storage_fixture_main
#include "../auth_storage.cpp"
#undef main
#include <arpa/inet.h>
struct IPAddress{bool fromString(const char *ip){in_addr address;return ip&&inet_pton(AF_INET,ip,&address)==1;}};
static bool knownFields(JsonObjectConst object,const char *allowed){
 for(JsonPairConst entry:object){const std::string key=std::string("|")+entry.key().c_str()+"|";if(!strstr(allowed,key.c_str()))return false;}return true;
}
static bool validSection(const String&,JsonObjectConst){return true;}
namespace iotcloud{struct HubService{static bool valid(JsonVariantConst){return true;}};}
#include "route-valid.inc"
static const char *TC1="aabbccddee01",*S30="aabbccddee02",*KEY="1234567890abcdef1234567890abcdef";
static std::string configSnapshot(){std::string text;serializeJson(config,text);return text;}
static bool pairStore(const char *peer,const char *ip,const char *key=KEY){
 lanPairRoutePeer=peer;lanPairRouteIP=ip;
 const bool result=ph_auth_remote_store(&localAuth,peer,key,PH_AUTH_SCOPE_CONTROL,ph_auth_credential_epoch(&localAuth));
 lanPairRoutePeer=nullptr;lanPairRouteIP=nullptr;return result;
}
static void setup(){
 prefs.values.clear();prefs.mode=Preferences::OK;legacy="{}";boot();
 DynamicJsonDocument next(configCapacity);next.set(config);auto fixed=next.createNestedArray("lan_peers").createNestedObject();fixed["peer"]=TC1;fixed["ip"]="192.0.2.10";assert(saveConfig(next));
 assert(ph_auth_remote_store(&localAuth,TC1,KEY,PH_AUTH_SCOPE_CONTROL,ph_auth_credential_epoch(&localAuth)));
}
int main(){
 setup();const auto original=configSnapshot();const ph_auth_store originalStore=localAuth.store;
 assert(pairStore(S30,"192.0.2.20"));assert(lanRouteMatches(S30,"192.0.2.20")&&config["lan_peers"][1]["pair_created"]==true);
 assert(validConfig(config.as<JsonObjectConst>()));boot();assert(lanRouteMatches(S30,"192.0.2.20"));
 assert(ph_auth_remote_remove(&localAuth,S30));assert(configSnapshot()==original&&!memcmp(&localAuth.store,&originalStore,sizeof(originalStore)));
 boot();assert(configSnapshot()==original&&!memcmp(&localAuth.store,&originalStore,sizeof(originalStore)));
 puts("authenticated new pin+key -> reboot -> unpair: exact old config+TC1 table PASS");
 // Old imported fixed-IP pins remain after explicit update and revocation.
 assert(pairStore(TC1,"192.0.2.30","abcdef1234567890abcdef1234567890"));
 assert(config["lan_peers"].size()==1&&!config["lan_peers"][0].containsKey("pair_created"));
 assert(ph_auth_remote_remove(&localAuth,TC1));assert(lanRouteMatches(TC1,"192.0.2.30"));
 puts("old fixed pin update and unpair preserve imported pin semantics PASS");
 setup();const auto before=prefs.values;const auto beforeConfig=configSnapshot();const auto beforeStore=localAuth.store;
 prefs.mode=Preferences::FAIL_OLD;assert(!pairStore(S30,"192.0.2.20"));
 assert(configSnapshot()==beforeConfig&&!memcmp(&localAuth.store,&beforeStore,sizeof(beforeStore))&&prefs.values==before&&configRecovery);
 prefs.mode=Preferences::OK;boot();assert(!lanRouteMatches(S30,"192.0.2.20"));assert(!lanRouteRecord(localAuth.store,S30));
 // An uncertain write may reload a complete new generation, never half.
 prefs.mode=Preferences::FAIL_NEW;assert(!pairStore(S30,"192.0.2.20"));
 assert(configSnapshot()==beforeConfig&&!memcmp(&localAuth.store,&beforeStore,sizeof(beforeStore))&&configRecovery);
 prefs.mode=Preferences::OK;boot();assert(lanRouteMatches(S30,"192.0.2.20")&&lanRouteRecord(localAuth.store,S30));
 const auto pairedConfig=configSnapshot();const auto pairedStore=localAuth.store;const auto pairedBytes=prefs.values;
 prefs.mode=Preferences::FAIL_OLD;assert(!ph_auth_remote_remove(&localAuth,S30));
 assert(configSnapshot()==pairedConfig&&!memcmp(&localAuth.store,&pairedStore,sizeof(pairedStore))&&prefs.values==pairedBytes);
 prefs.mode=Preferences::OK;boot();assert(ph_auth_remote_remove(&localAuth,S30));assert(configSnapshot()==beforeConfig);
 puts("NVS failed save/revoke keep both RAM parts old; reload sees whole generation PASS");
 // Staged routing cannot bleed into an unrelated inbound-pair write.
 setup();lanPairRoutePeer=S30;lanPairRouteIP="192.0.2.20";char key[33];
 assert(ph_auth_pair(&localAuth,"","aabbccddee03",PH_AUTH_SCOPE_SENSOR,key));
 lanPairRoutePeer=nullptr;lanPairRouteIP=nullptr;assert(!lanRouteMatches(S30,"192.0.2.20"));
 DynamicJsonDocument imported(2048);auto peer=imported.createNestedArray("lan_peers").createNestedObject();peer["peer"]=S30;peer["ip"]="192.0.2.20";
 assert(validConfig(imported.as<JsonObjectConst>()));peer["pair_created"]=true;assert(validConfig(imported.as<JsonObjectConst>()));
 for(const char *wire:{"false","1","\"true\"","null"}){DynamicJsonDocument value(128);assert(!deserializeJson(value,wire));peer["pair_created"].set(value.as<JsonVariantConst>());assert(!validConfig(imported.as<JsonObjectConst>()));}
 peer["pair_created"]=true;peer["unexpected"]=true;assert(!validConfig(imported.as<JsonObjectConst>()));
 puts("old two-field imports and true marker accepted; malformed marker/fields denied PASS");
}
