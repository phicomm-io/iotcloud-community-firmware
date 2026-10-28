#include <ArduinoJson.h>
#include "local_auth.h"
#include <string>
#include <memory>
#include <map>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
struct String:std::string {using std::string::string;String(const std::string&s):std::string(s){}bool reserve(size_t n){std::string::reserve(n);return true;}size_t write(uint8_t c){push_back(char(c));return 1;}size_t write(const uint8_t*p,size_t n){append(reinterpret_cast<const char*>(p),n);return n;}};
namespace ArduinoJson {template<>struct Converter<String>{static void toJson(const String&s,JsonVariant v){v.set(static_cast<const std::string&>(s));}};}
static DynamicJsonDocument config(16384);static bool configRecovery=false,localAuthReady=false;static ph_auth_state localAuth;static ph_auth_store loadedAuthStore;static char webPassword[33]={};
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
struct Preferences {
 std::map<std::string,std::string> values;enum Mode{OK,FAIL_OLD,FAIL_NEW,FAIL_UNKNOWN} mode=OK;
 bool isKey(const char*k){return values.count(k);}
 size_t getBytesLength(const char*k){return values[k].size();}
 size_t getBytes(const char*k,void*p,size_t n){auto&s=values[k];if(n>s.size())return 0;memcpy(p,s.data(),n);return n;}
 size_t putBytes(const char*k,const void*p,size_t n){if(mode==FAIL_OLD)return 0;if(mode==FAIL_UNKNOWN){values[k]="corrupt";return 0;}values[k].assign(static_cast<const char*>(p),n);return mode==FAIL_NEW?0:n;}
} prefs;
static String legacy="{}";static String loadLegacyConfigText(){return legacy;}
#include "../src/auth_storage.inc"
#include "transaction.inc"
static void boot(){localAuthReady=false;configRecovery=false;config=DynamicJsonDocument(16384);String wire=loadAuthConfigText();auto error=deserializeJson(config,wire.c_str(),wire.length());if(error)fprintf(stderr,"boot wire %s error %s\n",wire.c_str(),error.c_str());assert(!error);updatePassword();initLocalAuth();assert(localAuthReady&&!configRecovery);}
int main(){
 (void)hubRestartPending;boot();char key[33],remote[33],token[33];
 assert(ph_auth_pair(&localAuth,"","aabbccddee01",2,key));assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));
 assert(ph_auth_remote_store(&localAuth,"aabbccddee02",key,2,ph_auth_credential_epoch(&localAuth)));
 auto original=prefs.values;boot();assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));unsigned scopes;assert(ph_auth_remote_get(&localAuth,"aabbccddee02",remote,&scopes)&&scopes==2);
 // Config edits preserve both directions; password changes atomically clear them.
 DynamicJsonDocument next(16384);next=DynamicJsonDocument(16384);next.set(config);next["device"]["name"]="newname";assert(saveConfig(next));boot();assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));
 prefs.mode=Preferences::FAIL_OLD;next=DynamicJsonDocument(16384);next.set(config);next["auth"]["password"]="adminfjr";assert(!saveConfig(next));assert(!configRecovery&&String(webPassword)=="");assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));boot();assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));
 prefs.mode=Preferences::FAIL_NEW;next=DynamicJsonDocument(16384);next.set(config);next["auth"]["password"]="adminfjr";assert(saveConfig(next));assert(String(webPassword)=="adminfjr");prefs.mode=Preferences::OK;boot();assert(!ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));assert(!ph_auth_remote_get(&localAuth,"aabbccddee02",remote,&scopes));
 assert(ph_auth_pair(&localAuth,"adminfjr","aabbccddee01",2,key));assert(ph_auth_http_login(&localAuth,"adminfjr",token));char header[80];snprintf(header,sizeof(header),"Bearer %s",token);
 next=DynamicJsonDocument(16384);next.set(config);assert(saveConfigTransaction(next,true));ph_auth_context ctx;assert(!ph_auth_http_check(&localAuth,header,&ctx));boot();assert(!ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token));
 assert(ph_auth_pair(&localAuth,"adminfjr","aabbccddee01",2,key));prefs.mode=Preferences::FAIL_UNKNOWN;next=DynamicJsonDocument(16384);next.set(config);next["auth"]["password"]="replacement";assert(!saveConfig(next)&&configRecovery);assert(device_local_auth()==nullptr);prefs.mode=Preferences::OK;assert(!saveConfig(next));
 // The corrupt authoritative blob never falls back to legacy old credentials.
 legacy="{\"auth\":{\"password\":\"old-password\"}}";assert(loadAuthConfigText()=="invalid");prefs.values=original;boot();assert(ph_auth_pair_resume(&localAuth,"aabbccddee01",key,token)==false);
 assert(resetConfiguration());boot();assert(localAuth.store.count==0&&localAuth.store.remote_count==0&&String(webPassword)=="");
 puts("production atomic config/pair storage PASS");
}
