#include <ArduinoJson.h>
#include "local_auth.h"
#include <string>
#include <memory>
#include <map>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
struct String:std::string {using std::string::string;String(const std::string&s):std::string(s){}bool reserve(size_t n){std::string::reserve(n);return true;}bool concat(const char*p,size_t n){append(p,n);return true;}size_t write(uint8_t c){push_back(char(c));return 1;}size_t write(const uint8_t*p,size_t n){append(reinterpret_cast<const char*>(p),n);return n;}};
namespace ArduinoJson {template<>struct Converter<String>{static void toJson(const String&s,JsonVariant v){v.set(static_cast<const std::string&>(s));}};}
static const size_t configCapacity=16384;static DynamicJsonDocument config(configCapacity);static bool configRecovery=false,localAuthReady=false;static ph_auth_state localAuth;static ph_auth_store loadedAuthStore;static char webPassword[33]={};
static int authMux;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
using SemaphoreHandle_t=void*;static SemaphoreHandle_t localAuthMutex;
#define portMAX_DELAY 1
static void*xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);}static void xSemaphoreTake(void*,int){}static void xSemaphoreGive(void*){}
static uint32_t now=1;static uint32_t millis(){return now;}
static void esp_fill_random(void*p,size_t n){static uint8_t counter;for(size_t i=0;i<n;i++)static_cast<uint8_t*>(p)[i]=++counter;}
static void updatePassword(){strncpy(webPassword,config["auth"]["password"]|"",32);webPassword[32]=0;}
static bool hubRestartPending(){return false;}
enum {PT_BLOB,PT_INVALID};
struct Preferences {
 std::map<std::string,std::string> values;enum Mode{OK,FAIL_OLD,FAIL_NEW,FAIL_UNKNOWN} mode=OK;
 int getType(const char*k){return values.count(k)?PT_BLOB:PT_INVALID;}
 bool isKey(const char*k){return values.count(k);}
 size_t getBytesLength(const char*k){return values[k].size();}
 size_t getBytes(const char*k,void*p,size_t n){auto&s=values[k];if(n>s.size())return 0;memcpy(p,s.data(),n);return n;}
 size_t putBytes(const char*k,const void*p,size_t n){if(mode==FAIL_OLD)return 0;if(mode==FAIL_UNKNOWN){values[k]="corrupt";return 0;}values[k].assign(static_cast<const char*>(p),n);return mode==FAIL_NEW?0:n;}
} prefs;
static String legacy="{}";static String loadLegacyConfigText(){return legacy;}
struct nvs_stats_t{size_t total_entries=10000,free_entries=10000,used_entries=0;};
static const int ESP_OK=0;static int nvs_get_stats(const char*,nvs_stats_t*s){*s=nvs_stats_t();return ESP_OK;}
#include "../src/auth_storage.inc"
static bool webPending=false,webDone=false,webAuthRequired=false,otaActive=false;static ph_auth_context webAuthContext;static int webMux=0,webOperation=0,hubHttpStatus=0;static String webResult,webBody,hubHttpToken;static char httpStatusBody[4096];static unsigned dispatched=0;
static String execute(const String&){++dispatched;return "executed";}static String backupExecute(){++dispatched;return "backup";}static void hubApiExecute(){++dispatched;}static bool mqttStateJson(String&,char*,int,bool){++dispatched;return true;}
static unsigned invalidations=0;static void invalidateBusinessStatusCache(){++invalidations;}static bool businessStatusReady=false;static String cachedHttpStatus(){return "cached";}
#include "transaction.inc"
static void boot(){localAuthReady=false;configRecovery=false;config=DynamicJsonDocument(16384);String wire=loadAuthConfigText();auto error=deserializeJson(config,wire.c_str(),wire.length());if(error)fprintf(stderr,"boot wire %s error %s\n",wire.c_str(),error.c_str());assert(!error);config.shrinkToFit();updatePassword();initLocalAuth();assert(localAuthReady&&!configRecovery);}
static void compactCodec(){
 ph_auth_store original,decoded;ph_auth_store_empty(&original);
 uint8_t bytes[4+32*24];authStoreEncode(bytes,original);assert(authStoreWireSize(original)==4&&authStoreDecode(bytes,4,decoded));
 // Unoccupied dirty slots do not become credentials; holes within count fail.
 memset(&original.pairs[11],0xcd,sizeof(original.pairs[11]));authStoreEncode(bytes,original);assert(authStoreDecode(bytes,4,decoded)&&decoded.count==0);
 ph_auth_store_empty(&original);original.count=original.remote_count=12;
 for(unsigned i=0;i<12;i++){auto &a=original.pairs[i];snprintf(a.peer,sizeof(a.peer),"aabbccdd%04x",i);memset(a.key,i+1,16);a.scopes=7;original.remote[i]=a;}
 assert(ph_auth_store_valid(&original));authStoreEncode(bytes,original);const size_t n=authStoreWireSize(original);assert(n==772&&authStoreDecode(bytes,n,decoded)&&!memcmp(&original,&decoded,sizeof(original)));
 for(size_t cut=0;cut<n;cut++)assert(!authStoreDecode(bytes,cut,decoded));assert(!authStoreDecode(bytes,n+1,decoded));
 auto reject=[&](size_t at,uint8_t value){uint8_t saved=bytes[at];bytes[at]=value;assert(!authStoreDecode(bytes,n,decoded));bytes[at]=saved;};
 reject(0,13);reject(1,255);reject(2,13);reject(3,255);reject(4,0);reject(4+12,255);reject(4+29,0);reject(4+29,8);reject(4+30,1);reject(4+12*32+29,8);
 uint8_t saved[32];memcpy(saved,bytes+4+32,32);memcpy(bytes+4+32,bytes+4,32);assert(!authStoreDecode(bytes,n,decoded));memcpy(bytes+4+32,saved,32);
 uint8_t key[16];memcpy(key,bytes+4+13,16);memset(bytes+4+13,0,16);assert(!authStoreDecode(bytes,n,decoded));memcpy(bytes+4+13,key,16);
 memset(saved,0,sizeof(saved));memcpy(saved,bytes+4+32,32);memset(bytes+4+32,0,32);assert(!authStoreDecode(bytes,n,decoded));memcpy(bytes+4+32,saved,32);
 memcpy(saved,bytes+4+13*32,32);memcpy(bytes+4+13*32,bytes+4+12*32,32);assert(!authStoreDecode(bytes,n,decoded));memcpy(bytes+4+13*32,saved,32);
 // Invalid authoritative v3 never falls back to legacy credentials.
 const auto previous=prefs.values;std::string raw(sizeof(AuthConfigHeader)+n+1,0);AuthConfigHeader h{authConfigMagic,3,0,1};memcpy(&raw[0],&h,sizeof(h));memcpy(&raw[sizeof(h)],bytes,n);raw.back()=char(0x80);prefs.values[authConfigKey]=raw;assert(loadAuthConfigText()!="invalid");
 for(size_t cut=0;cut<raw.size();cut++){prefs.values[authConfigKey]=raw.substr(0,cut);assert(loadAuthConfigText()=="invalid");}
 h.length=UINT32_MAX;memcpy(&raw[0],&h,sizeof(h));prefs.values[authConfigKey]=raw;assert(loadAuthConfigText()=="invalid");prefs.values=previous;
}
int main(){
 compactCodec();
 (void)hubRestartPending;boot();char key[33],remote[33],token[33];
 assert(ph_auth_pair(&localAuth,"","aabbccddee01",2,key));assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));
 assert(ph_auth_remote_store(&localAuth,"aabbccddee02",key,2,ph_auth_credential_epoch(&localAuth)));
 auto original=prefs.values;boot();assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));unsigned scopes;assert(ph_auth_remote_get(&localAuth,"aabbccddee02",remote,&scopes)&&scopes==2);
 // Config edits preserve both directions; password changes atomically clear them.
 DynamicJsonDocument next(16384);next=DynamicJsonDocument(16384);next.set(config);next["device"]["name"]="newname";assert(saveConfig(next));boot();assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));
 std::string beforeFailedConfig;serializeJson(config,beforeFailedConfig);unsigned beforeFailedInvalidations=invalidations;
 prefs.mode=Preferences::FAIL_OLD;next=DynamicJsonDocument(16384);next.set(config);next["auth"]["password"]="adminfjr";assert(!saveConfig(next));assert(configRecovery&&String(webPassword)=="");std::string afterFailedConfig;serializeJson(config,afterFailedConfig);assert(afterFailedConfig==beforeFailedConfig&&invalidations==beforeFailedInvalidations);assert(device_local_auth()==nullptr);boot();assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));
 prefs.mode=Preferences::FAIL_NEW;next=DynamicJsonDocument(16384);next.set(config);next["auth"]["password"]="adminfjr";assert(!saveConfig(next)&&configRecovery);assert(String(webPassword)=="");prefs.mode=Preferences::OK;boot();assert(!ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));assert(!ph_auth_remote_get(&localAuth,"aabbccddee02",remote,&scopes));
 assert(ph_auth_pair(&localAuth,"adminfjr","aabbccddee01",2,key));assert(ph_auth_http_login(&localAuth,"adminfjr",token));char header[80];snprintf(header,sizeof(header),"Bearer %s",token);
 next=DynamicJsonDocument(16384);next.set(config);assert(saveConfigTransaction(next,true));ph_auth_context ctx;assert(!ph_auth_http_check(&localAuth,header,&ctx));boot();assert(!ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));
 assert(ph_auth_pair(&localAuth,"adminfjr","aabbccddee01",2,key));assert(ph_auth_http_login(&localAuth,"adminfjr",token));snprintf(header,sizeof(header),"Bearer %s",token);ph_auth_context queued;assert(ph_auth_http_check(&localAuth,header,&queued));assert(localAuthContextValid(&queued));prefs.mode=Preferences::FAIL_UNKNOWN;next=DynamicJsonDocument(16384);next.set(config);next["auth"]["password"]="replacement";assert(!saveConfig(next)&&configRecovery);assert(device_local_auth()==nullptr);assert(ph_auth_context_valid(&queued,PH_AUTH_SCOPE_ADMIN));assert(!localAuthContextValid(&queued));for(const char *command:{"system.reset","system.reboot","state.get"}){webPending=true;webDone=false;webAuthRequired=true;webAuthContext=queued;webBody=command;dispatchQueued();assert(webDone&&!webPending&&dispatched==0&&webResult=="{\"code\":\"unauthorized\"}");}webOperation=4;webPending=true;dispatchQueued();assert(dispatched==0);prefs.mode=Preferences::OK;assert(!saveConfig(next));
 // The corrupt authoritative blob never falls back to legacy old credentials.
 legacy="{\"auth\":{\"password\":\"old-password\"}}";assert(loadAuthConfigText()=="invalid");prefs.values=original;boot();assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token)==false);
 assert(resetConfiguration());boot();assert(localAuth.store.count==0&&localAuth.store.remote_count==0&&String(webPassword)=="");
 // Migrate the previously integrated v1 JSON+auth envelope without losing keys.
 const std::string oldJson="{\"_version\":1,\"auth\":{\"password\":\"adminfjr\"}}";AuthConfigHeader blobHeader{authConfigMagic,1,0,uint32_t(oldJson.size())};ph_auth_store oldStore;ph_auth_store_empty(&oldStore);oldStore.count=oldStore.remote_count=1;strcpy(oldStore.pairs[0].peer,"aabbccddee11");strcpy(oldStore.remote[0].peer,"aabbccddee12");memset(oldStore.pairs[0].key,0x6b,16);memset(oldStore.remote[0].key,0x6b,16);oldStore.pairs[0].scopes=oldStore.remote[0].scopes=2;std::string oldBlob(sizeof(blobHeader)+sizeof(oldStore)+oldJson.size(),0);memcpy(&oldBlob[0],&blobHeader,sizeof(blobHeader));memcpy(&oldBlob[sizeof(blobHeader)],&oldStore,sizeof(oldStore));memcpy(&oldBlob[sizeof(blobHeader)+sizeof(oldStore)],oldJson.data(),oldJson.size());prefs.values[authConfigKey]=oldBlob;boot();assert(String(webPassword)=="adminfjr");next=DynamicJsonDocument(16384);next.set(config);assert(saveConfig(next));memcpy(&blobHeader,prefs.values[authConfigKey].data(),sizeof(blobHeader));assert(blobHeader.version==3);boot();assert(String(webPassword)=="adminfjr");assert(ph_auth_pair_resume(&localAuth,"aabbccddee11","6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b",token));assert(ph_auth_remote_get(&localAuth,"aabbccddee12",remote,&scopes)&&!strcmp(remote,"6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b"));
 // Previously released v2 fixed-auth MessagePack remains readable.
 String packed;assert(serializeWireMP(config,packed));AuthConfigHeader v2{authConfigMagic,2,0,uint32_t(packed.size())};std::string v2Blob(sizeof(v2)+sizeof(localAuth.store)+packed.size(),0);memcpy(&v2Blob[0],&v2,sizeof(v2));memcpy(&v2Blob[sizeof(v2)],&localAuth.store,sizeof(localAuth.store));memcpy(&v2Blob[sizeof(v2)+sizeof(localAuth.store)],packed.data(),packed.size());prefs.values[authConfigKey]=v2Blob;boot();assert(String(webPassword)=="adminfjr"&&localAuth.store.count==1&&localAuth.store.remote_count==1);next=DynamicJsonDocument(16384);next.set(config);assert(saveConfig(next));memcpy(&v2,prefs.values[authConfigKey].data(),sizeof(v2));assert(v2.version==3);
 puts("production atomic config/pair storage PASS");
}
