#define main cache_fixture_main
#include "status_cache.cpp"
#undef main
extern "C" {
#include "lan_wire_v4.h"
}
#include <vector>
static uint32_t sensorBoot=7;
static bool otaActive=false,localAuthReady=true;
static ph_auth_state localAuth={};
static bool credentialsValid=true;
extern "C" bool ph_auth_context_valid(const ph_auth_context *c,unsigned scopes){return credentialsValid&&c->owner==&localAuth&&(c->scopes&scopes)==scopes;}
extern "C" bool ph_auth_udp_check(ph_auth_state *,const char *peer,const char *token,ph_auth_context *c){
 if(!credentialsValid||token[0]!='1')return false;
 *c={};c->owner=&localAuth;c->source=PH_AUTH_UDP_PAIR;c->scopes=PH_AUTH_SCOPE_CONTROL;c->generation=1;strcpy(c->peer,peer);return true;
}
static void lan_auth_hex(const uint8_t token[16],char text[33]){for(unsigned i=0;i<16;i++)sprintf(text+2*i,"%02x",token[i]);}
struct IPAddress {bool fromString(const char*){return true;}};
static std::vector<unsigned char> sent;
static uint16_t sentPort;
struct {bool beginPacket(IPAddress,uint16_t p){sentPort=p;sent.clear();return true;}size_t write(const uint8_t *p,size_t n){sent.assign(p,p+n);return n;}int endPacket(){return 1;}}sensorUdp;
struct {void addService(const char*,const char*,unsigned){}template<class T>void addServiceTxt(const char*,const char*,const char*,const T&){} }MDNS;
static unsigned calls=0;
static String execute(const String &,bool){++calls;DynamicJsonDocument reply(4096);reply["code"]="ok";reply["data"]["large"]=std::string(1800,'x');String wire;serializeWireMP(reply,wire);return wire;}
#include "lan-business.inc"
static void input(lan_v4_packet &p){
 char bytes[1200];ph_writer w;ph_buf b;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,bytes,sizeof(bytes))&&lan_v4_encode(&w,&p));size_t n=phw_finish_fixed(&w);
 ph_value root;assert(phv_parse(PH_WIRE_MSGPACK,bytes,n,&root));assert(lanBusinessPacket(&root,"192.0.2.2",45454));assert(sentPort==45454);
}
static lan_v4_packet result(){lan_v4_packet p;assert(lan_v4_decode(sent.data(),sent.size(),&p));return p;}
int main(){
 deviceId="aabbccddeeff";sensorBoot=7;
 DynamicJsonDocument request(512);request["action"]="status.get";request.createNestedObject("params");request["request_id"]="r1";
 String body;serializeWireMP(request,body);lan_v4_packet p={};p.op=LAN_V4_COMMAND;strcpy(p.src,"001122334455");strcpy(p.dst,deviceId.c_str());memset(p.token,0x11,16);p.boot=3;p.id=1;p.body=body.c_str();p.body_length=body.length();
 input(p);assert(calls==1&&!strcmp(result().code,"ok")&&result().total==3);
 input(p);assert(calls==1);p.op=LAN_V4_PULL;p.body=nullptr;p.body_length=0;p.part=2;input(p);assert(result().part==2&&calls==1);
 p.op=LAN_V4_ACK;input(p);assert(!strcmp(result().code,"acked")&&!lanBusinessServer.response);
 p.op=LAN_V4_COMMAND;p.id=2;request["action"]="system.reset";body="";serializeWireMP(request,body);p.body=body.c_str();p.body_length=body.length();input(p);auto denied=result();assert(calls==1&&!strcmp(denied.code,"ok"));
 DynamicJsonDocument envelope(512);assert(!deserializeMsgPack(envelope,denied.body,denied.body_length));assert(envelope["code"]=="unauthorized"&&envelope["request_id"]=="r1");
 input(p);assert(calls==1);
 p.op=LAN_V4_ACK;p.body=nullptr;p.body_length=0;input(p);assert(!lanBusinessServer.response);
 p.op=LAN_V4_COMMAND;p.id=3;request["action"]="name.set";request["params"]["name"]="new name";
 body="";serializeWireMP(request,body);p.body=body.c_str();p.body_length=body.length();input(p);assert(calls==2);
 p.op=LAN_V4_ACK;p.body=nullptr;p.body_length=0;input(p);p.op=LAN_V4_COMMAND;p.id=4;request["action"]="config.set";
 body="";serializeWireMP(request,body);p.body=body.c_str();p.body_length=body.length();input(p);
 denied=result();envelope.clear();assert(!deserializeMsgPack(envelope,denied.body,denied.body_length));assert(calls==2&&envelope["code"]=="unauthorized");
 otaActive=true;pollLanBusiness();assert(!lanBusinessServer.response);p.id=5;input(p);assert(!strcmp(result().code,"busy"));
 otaActive=false;credentialsValid=false;input(p);assert(!strcmp(result().code,"auth_required"));pollLanBusiness();assert(!lanBusinessServer.response);
}
