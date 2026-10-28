// Real MiCO lan_encode -> DAIKIN production UDP handler -> real lan_decode.
#include <ArduinoJson.h>
#include "frames.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <string>
#include <cmath>
extern "C" {
#include "local_auth.h"
#include "typed_wire.h"
#include "lan_wire.h"
}
struct String:std::string {
 using std::string::string;String(const std::string &s):std::string(s){}
 bool reserve(size_t n){std::string::reserve(n);return true;}bool concat(const char*p,size_t n){append(p,n);return true;}
 void toLowerCase(){for(auto &c:*this)c=std::tolower(static_cast<unsigned char>(c));}
};
#include "wire_mp.h"
namespace ArduinoJson {template<> struct Converter<String>{static String fromJson(JsonVariantConst v){return v.as<std::string>();}static void toJson(const String&v,JsonVariant o){o.set(static_cast<const std::string&>(v));}};}
struct IPAddress {String address="127.0.0.1";String toString()const{return address;}bool fromString(const char*p){if(!p||strlen(p)>15||!strchr(p,'.'))return false;address=p;return true;}};
struct WiFiUDP {
 std::string incoming,outgoing;unsigned sent=0;IPAddress remote;
 int parsePacket()const{return incoming.size();}int available()const{return 0;}int read(){return -1;}
 int read(char*out,int n){incoming.copy(out,n);incoming.clear();return n;}IPAddress remoteIP()const{return remote;}
 bool beginPacket(IPAddress,unsigned){outgoing.clear();return true;}size_t write(const uint8_t*p,size_t n){outgoing.assign(reinterpret_cast<const char*>(p),n);return n;}int endPacket(){sent++;return 1;}
};
static uint32_t now=1;static uint32_t millis(){return now;}static unsigned randomCounter;
static void esp_fill_random(void*p,size_t n){for(size_t i=0;i<n;i++)static_cast<uint8_t*>(p)[i]=uint8_t(++randomCounter);}
static String deviceId="9c9c1ff76748";static constexpr size_t configCapacity=16384;
static DynamicJsonDocument config(configCapacity);static bool configRecovery=false,saveFailed=false;
static char currentPassword[33]={};static ph_auth_state localAuth;static bool localAuthReady=true;static ph_auth_store persisted;
static bool saveConfig(DynamicJsonDocument &d){if(saveFailed)return false;config.set(d);return true;}
static ph_auth_state *activeAuth=&localAuth;
extern "C" ph_auth_state *device_local_auth(){return activeAuth;}
enum Sensor {PM25,CO2,TEMPERATURE,HUMIDITY,TVOC,HCHO,PM10,SENSOR_COUNT};
static daikin::Sample readings[SENSOR_COUNT];
static const char *keys[]={"pm25","co2","temperature","humidity","tvoc","hcho","pm10"},*units[]={"ug/m3","ppm","C","%","ppb","mg/m3","ug/m3"};
static bool sensorPresent(unsigned){return true;}
#include "../src/lan_pairing.inc"
#include "../src/sessions.inc"
namespace remote {
#define lan_auth_peer remote_lan_auth_peer
#define lan_auth_secret remote_lan_auth_secret
#define lan_auth_hex remote_lan_auth_hex
#define AUTH_DEVICE_ID "aabbccddee03"
#include "lan_auth.inc"
#undef AUTH_DEVICE_ID
#undef lan_auth_peer
#undef lan_auth_secret
#undef lan_auth_hex
}
static std::string remoteReply(ph_auth_state &server,const std::string &packet){char bytes[384];ph_buf buffer;ph_writer writer;ph_value request;assert(phv_parse(PH_WIRE_MSGPACK,packet.data(),packet.size(),&request));assert(phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,bytes,sizeof(bytes)));activeAuth=&server;assert(remote::feature_lan_pair(&request,&writer,"127.0.0.1"));activeAuth=&localAuth;return std::string(bytes,phw_finish_fixed(&writer));}
static std::string encode(lan_packet &p){char bytes[512];ph_buf b;ph_writer w;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,bytes,sizeof(bytes)));assert(lan_encode(&w,&p));size_t n=phw_finish_fixed(&w);assert(n);return std::string(bytes,n);}
static std::string authPacket(const char *op,const char *peer,const char *field,const char *secret,uint8_t nonceValue=7){
 char bytes[256];ph_buf b;ph_writer w;uint8_t nonce[16];memset(nonce,nonceValue,16);assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,bytes,sizeof(bytes)));phw_begin(&w,nullptr,false);phw_text(&w,"op",op);phw_text(&w,"src",peer);phw_bytes_value(&w,"nonce",nonce,16);
 if(!strcmp(field,"password")){phw_text(&w,field,secret);phw_uint(&w,"scopes",2);}else{uint8_t token[16];assert(ph_auth_token_decode(secret,token));phw_bytes_value(&w,field,token,16);}phw_end(&w);return std::string(bytes,phw_finish_fixed(&w));
}
static std::string deliver(const std::string &packet){sensorUdp.incoming=packet;unsigned sent=sensorUdp.sent;pollSessions();return sensorUdp.sent!=sent?sensorUdp.outgoing:std::string();}
static String grant(const char *token){char bytes[256];uint8_t raw[16];assert(ph_auth_token_decode(token,raw));ph_buf buffer;ph_writer writer;assert(phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,bytes,sizeof(bytes)));phw_begin(&writer,nullptr,false);phw_text(&writer,"action","sensor.session.set");phw_begin(&writer,"params",false);phw_uint(&writer,"v",3);phw_text(&writer,"a1","aabbccddee01");phw_u32(&writer,"boot",UINT32_MAX);phw_bytes_value(&writer,"token",raw,16);phw_end(&writer);phw_end(&writer);return String(bytes,phw_finish_fixed(&writer));}
static std::string secretReply(const std::string &wire,const char *key){ph_value root,v;const void*bytes;size_t n;assert(phv_parse(PH_WIRE_MSGPACK,wire.data(),wire.size(),&root));assert(phv_get(&root,"code",&v)&&phv_equal_text(&v,"ok"));assert(phv_get(&root,key,&v)&&phv_bytes(&v,&bytes,&n)&&n==16);char out[33];lan_auth_hex(static_cast<const uint8_t*>(bytes),out);return out;}
int main(){
 ph_auth_store_empty(&persisted);ph_auth_ops ops={};ops.now=[](void*){return now;};ops.random=[](void*,uint8_t*b,size_t n){esp_fill_random(b,n);return true;};ops.password=[](void*,char out[33]){strcpy(out,currentPassword);return true;};ops.save=[](void*,const ph_auth_store*s){if(saveFailed)return false;persisted=*s;return true;};assert(ph_auth_init(&localAuth,&ops,&persisted));
 sensorBoot=UINT32_MAX;for(auto &r:readings){r.received=true;r.at=now;}readings[TEMPERATURE].value=-3.125f;readings[HUMIDITY].value=45.5f;readings[PM25].value=12.125f;readings[HCHO].value=.123f;readings[CO2].value=625;
 lan_packet read={};read.op=LAN_READ;strcpy(read.src,"aabbccddee01");strcpy(read.dst,deviceId.c_str());read.boot=UINT32_MAX;read.id=0x80000000;memset(read.token,0xab,16);
 assert(deliver(R"({"v":2,"op":"read","src":"aabbccddee01","token":"abababababababababababababababab"})").empty());assert(deliver(R"({"op":"auth_pair","src":"aabbccddee01","password":""})").empty());
 assert(deliver(encode(read)).empty());assert(sessionGrant(grant("abababababababababababababababab")));
 lan_packet sample;auto wire=deliver(encode(read));assert(lan_decode(wire.data(),wire.size(),&sample));assert(sample.boot==UINT32_MAX&&sample.id==0x80000000&&sample.sample.fields==31);assert(sample.sample.value[0]==-3125&&sample.sample.value[1]==45500&&sample.sample.value[2]==12125&&sample.sample.value[3]==123000&&sample.sample.value[4]==625);
 now+=1000;wire=deliver(encode(read));assert(lan_decode(wire.data(),wire.size(),&sample)&&sample.sample.age_ms[0]==1000);
 readings[HCHO].received=false;wire=deliver(encode(read));assert(lan_decode(wire.data(),wire.size(),&sample)&&!(sample.sample.fields&8));
 read.op=LAN_PROBE;wire=deliver(encode(read));assert(lan_decode(wire.data(),wire.size(),&sample)&&sample.op==LAN_PONG);
 read.boot=0;assert(deliver(encode(read)).empty());read.boot=UINT32_MAX;
 // Empty-password local pair uses the same core; key is not a live token.
 const char *peer="aabbccddee02";auto pair=authPacket("auth_pair",peer,"password","");auto key=secretReply(deliver(pair),"key");auto token=secretReply(deliver(authPacket("auth_resume",peer,"key",key.c_str())),"token");
 strcpy(read.src,peer);assert(ph_auth_token_decode(key.c_str(),read.token));assert(deliver(encode(read)).empty());assert(ph_auth_token_decode(token.c_str(),read.token));assert(!deliver(encode(read)).empty());
 auto unpair=authPacket("auth_unpair",peer,"token",token.c_str());auto ack=deliver(unpair);assert(!ack.empty());assert(deliver(encode(read)).empty());assert(deliver(unpair)==ack); // lost ACK retransmission is idempotent
 assert(deliver(authPacket("auth_unpair",peer,"token",token.c_str(),8))!=ack);
 now+=30000;assert(deliver(unpair)!=ack);
 // Receiver password pairing and failed durable write never create a grant.
 assert(ph_auth_credential_commit(&localAuth,"adminfjr",false,[](void*,const ph_auth_store*s){persisted=*s;strcpy(currentPassword,"adminfjr");return true;},nullptr));
 wire=deliver(pair);ph_value root,v;assert(phv_parse(PH_WIRE_MSGPACK,wire.data(),wire.size(),&root)&&phv_get(&root,"code",&v)&&phv_equal_text(&v,"auth_required"));
 key=secretReply(deliver(authPacket("auth_pair",peer,"password","adminfjr")),"key");assert(ph_auth_init(&localAuth,&ops,&persisted));token=secretReply(deliver(authPacket("auth_resume",peer,"key",key.c_str())),"token");
 assert(ph_auth_token_decode(token.c_str(),read.token));assert(!deliver(encode(read)).empty());
 // The independent MQTT grant survives local credential rotation.
 assert(!deliver(encode(read)).empty());strcpy(read.src,"aabbccddee01");memset(read.token,0xab,16);assert(!deliver(encode(read)).empty());
 // Real DAIKIN asynchronous sender -> actual shared receiver -> DAIKIN handler.
 char remoteKey[33];unsigned scopes;ph_auth_state remoteAuth;ph_auth_store empty;ph_auth_store_empty(&empty);assert(ph_auth_init(&remoteAuth,&ops,&empty));
 const char *code=nullptr;assert(lanPairStart("aabbccddee03","10.0.0.3","adminfjr",7,&code)&&!strcmp(code,"accepted"));pollLanPairing();auto request=sensorUdp.outgoing;
 sensorUdp.remote.address="10.0.0.3";auto response=remoteReply(remoteAuth,request);deliver(response);assert(lc_find(&lanClient,"aabbccddee03",false)->phase==LC_RESUME);
 pollLanPairing();request=sensorUdp.outgoing;response=remoteReply(remoteAuth,request);deliver(response);assert(lc_find(&lanClient,"aabbccddee03",false)->phase==LC_READY);
 // Reboot destroys RAM token, reloads remote key and resumes automatically.
 lanClient=lc_state();pollLanPairing();assert(lc_find(&lanClient,"aabbccddee03",false)->phase==LC_RESUME);request=sensorUdp.outgoing;response=remoteReply(remoteAuth,request);deliver(response);assert(lc_find(&lanClient,"aabbccddee03",false)->phase==LC_READY);
 assert(lanUnpairStart("aabbccddee03",&code));pollLanPairing();request=sensorUdp.outgoing;response=remoteReply(remoteAuth,request);/* drop first successful ACK */now+=1000;pollLanPairing();assert(sensorUdp.outgoing==request);response=remoteReply(remoteAuth,sensorUdp.outgoing);deliver(response);assert(!ph_auth_remote_get(&localAuth,"aabbccddee03",remoteKey,&scopes));assert(!strcmp(lc_find(&lanClient,"aabbccddee03",false)->code,"unpaired"));
 configRecovery=true;assert(deliver(encode(read)).empty());assert(deliver(pair).empty());assert(deliver(authPacket("auth_resume",peer,"key",key.c_str())).empty());configRecovery=false;
 puts("production DAIKIN native v3/auth pairing interop PASS");
}
