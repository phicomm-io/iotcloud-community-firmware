// Exercise the production session adapter with the pinned ArduinoJson codec.
// Only the clock, Arduino String and UDP transport are replaced on the host.
#include <ArduinoJson.h>
#include "frames.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <string>

struct String:std::string {
  using std::string::string;
  String(const std::string &value):std::string(value){}
  void toLowerCase(){for(auto &c:*this)c=std::tolower(static_cast<unsigned char>(c));}
};
namespace ArduinoJson {
template<> struct Converter<String> {
  static String fromJson(JsonVariantConst value){return value.as<std::string>();}
  static void toJson(const String &value,JsonVariant out){out.set(value.c_str());}
};
}
struct IPAddress {
  unsigned value=0;
  String toString()const{return value?"127.0.0.1":"0.0.0.0";}
};
struct WiFiUDP {
  std::string incoming,outgoing;
  unsigned sent=0;
  int parsePacket()const{return incoming.size();}
  int available()const{return 0;}
  int read(){return -1;}
  int read(char *out,int n){incoming.copy(out,n);incoming.clear();return n;}
  IPAddress remoteIP()const{IPAddress ip;ip.value=1;return ip;}
  void beginPacket(IPAddress,unsigned){outgoing.clear();}
  void write(const uint8_t *p,size_t n){outgoing.assign(reinterpret_cast<const char *>(p),n);}
  void endPacket(){++sent;}
};
static uint32_t now;
static uint32_t millis(){return now;}
static String deviceId="9c9c1ff76748";
static const unsigned SENSOR_COUNT=1;
static daikin::Sample readings[SENSOR_COUNT];
static const char *keys[]={"pm25"},*units[]={"µg/m³"};
static bool sensorPresent(unsigned){return true;}
#include "../src/sessions.inc"

static String grant(const char *boot){
  return std::string("{\"action\":\"sensor.session.set\",\"params\":{\"v\":2,\"a1\":\"aabbccddee01\",\"boot\":")+boot+
    ",\"token\":\"abababababababababababababababab\"}}";
}
static void readSession(const char *boot,const char *id="-2147483648"){
  sensorUdp.incoming=std::string("{\"v\":2,\"src\":\"aabbccddee01\",\"dst\":\"9c9c1ff76748\",\"op\":\"read\",\"boot\":")+boot+
    ",\"id\":"+id+",\"token\":\"abababababababababababababababab\"}";
  pollSessions();
}
int main(){
  const struct {const char *wire;uint32_t bits;} counters[]={
    {"1",1},{"2147483647",INT32_MAX},{"-2147483648",0x80000000u},
    {"-1",UINT32_MAX},{"4294967295",UINT32_MAX}
  };
  sensorBoot=UINT32_MAX;readings[0].set(12,now);
  for(const auto &value:counters){
    assert(sessionGrant(grant(value.wire))&&sessions[0].boot==value.bits);
    unsigned before=sensorUdp.sent;
    readSession(value.wire);
    assert(sensorUdp.sent==before+1&&sessions[0].active);
    DynamicJsonDocument reply(4096);
    assert(!deserializeJson(reply,sensorUdp.outgoing));
    assert(reply["boot"].as<int32_t>()==-1);
    assert(reply["id"].as<int32_t>()==INT32_MIN);
    assert(reply["body"]["boot"].is<uint32_t>()&&reply["body"]["boot"].as<uint32_t>()==UINT32_MAX);
    assert(reply["body"]["model"]=="daikin");
    assert(reply["body"]["sensors"]["pm25"]["value"].as<int>()==12);
  }
  // A retry with the same bits must preserve the established session.
  assert(sessionGrant(grant("-1"))&&sessions[0].active);
  assert(sessionGrant(grant("4294967295"))&&sessions[0].active);
  DynamicJsonDocument status(1024);sessionStatus(status.to<JsonObject>());
  assert(status["max_sessions"]==4&&status["peers"].size()==1&&status["peers"][0]["session_ready"]==true);
  // Reauthorizing an idle peer must not imply a new UDP connection.
  now+=15000;
  assert(sessionGrant(grant("-1")));
  sessionStatus(status.to<JsonObject>());
  assert(status["peers"][0]["session_ready"]==false&&status["peers"][0]["idle_ms"]==15000);
  readSession("-1");sessionStatus(status.to<JsonObject>());
  assert(status["peers"][0]["session_ready"]==true&&status["peers"][0]["idle_ms"]==0);
  for(const char *bad:{"0","4294967296","-2147483649","1.5","\"1\"","null","true"}){
    assert(!sessionGrant(grant(bad))&&sessions[0].boot==UINT32_MAX&&sessions[0].active);
  }
  for(const char *bad:{"4294967296","-2147483649","1.5","\"1\"","null","true"}){
    unsigned before=sensorUdp.sent;
    readSession(bad);readSession("-1",bad);
    assert(sensorUdp.sent==before);
  }
  // Both unsigned and signed representations echo the same request bits.
  readSession("-1","4294967295");
  DynamicJsonDocument reply(4096);
  assert(!deserializeJson(reply,sensorUdp.outgoing)&&reply["id"].as<int32_t>()==-1);
  sensorSequence=INT32_MAX;
  DynamicJsonDocument body(1024);sensorBody(body.to<JsonObject>());
  assert(body["sequence"].is<uint32_t>()&&body["sequence"].as<uint32_t>()==0x80000000u);
  std::string feed;serializeJson(body,feed);
  assert(feed.find("\"boot\":4294967295")!=std::string::npos);
  assert(feed.find("\"sequence\":2147483648")!=std::string::npos);
  sensorSequence=UINT32_MAX-1;sensorBody(body.to<JsonObject>());
  feed.clear();serializeJson(body,feed);
  assert(feed.find("\"sequence\":4294967295")!=std::string::npos);
  sensorSequence=UINT32_MAX;sensorBody(body.to<JsonObject>());
  assert(body["sequence"].is<uint32_t>()&&body["sequence"].as<uint32_t>()==1);
  now+=1800000;unsigned before=sensorUdp.sent;readSession("-1");
  assert(sensorUdp.sent==before);
  assert(sessionGrant(grant("-1")));
  sessionStatus(status.to<JsonObject>());
  assert(status["peers"][0]["session_ready"]==false&&status["peers"][0]["ip"]=="");
  puts("PASS signed/unsigned counters, authenticated UDP, invalid ranges, idempotent grants, wraparound, expiry and UDP-only readiness");
}
