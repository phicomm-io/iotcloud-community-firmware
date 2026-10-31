// Compile the production command dispatcher; only SDK and durable-save edges
// are adapters. The existing auth-storage suite exercises that save edge.
#define WiFi statusWiFi
#define main status_cache_main
#include "status_cache.cpp"
#undef main
#undef WiFi
#include "device_name.h"
#include "climate.h"
#include "IoTCloudJson.h"
extern "C" {
#include "i18n.h"
}
struct : decltype(statusWiFi) {
  int scanComplete(){return 0;} void scanDelete(){} void scanNetworks(bool){}
  const char *SSID(int){return "";} int RSSI(int){return -50;} unsigned encryptionType(int){return 0;}
} WiFi;
static const int WIFI_SCAN_RUNNING=-1,WIFI_AUTH_OPEN=0;
static String lastRequest[8],lastBody[8],lastResponse[8];
static unsigned requestSlot=0,saves=0;
static constexpr size_t configCapacity=16384;
static bool mqttRestart=false,wifiRestart=false,saveReady=true;
static uint32_t nextSample=0,rebootAt=0;
static bool hubRestartPending(){return false;}
struct IPAddress{bool fromString(const char *){return true;}};
static bool lc_peer(const char *){return true;}
static void sessionStatus(JsonObject){}
static void lanPairStart(const char*,const char*,const char*,unsigned,const char**){}
static void lanUnpairStart(const char*,const char**){}
static bool lanPairStatus(const char*,JsonObject){return false;}
static bool homeFactoryReset(){return true;}
static bool resetConfiguration(){return true;}
static void hubRestoreChanged(){}
static void hubUpstreamChanged(){}
static bool saveConfig(DynamicJsonDocument &candidate){
  ++saves;
  if(!saveReady || candidate.overflowed())return false;
  assert(candidate["mqtt"]["host"]=="broker"&&candidate["auth"]["password"]=="unchanged");
  assert(candidate["lan_peers"][0]["peer"]=="02c400000001");
  config.set(candidate);invalidateBusinessStatusCache();return true;
}
#include "commands-execute.inc"
static DynamicJsonDocument call(const std::string &name,bool extra=false,const char *action="name.set",const char *id=nullptr){
  DynamicJsonDocument request(12000);request["action"]=action;
  const auto generated=std::string("name_")+std::to_string(requestSlot)+"_"+std::to_string(saves);
  request["request_id"]=id?id:generated.c_str();request["params"]["name"]=name;
  if(extra)request["params"]["language"]="en";
  String body;serializeWireMP(request,body);const auto result=execute(body);
  DynamicJsonDocument reply(12000);assert(!deserializeMsgPack(reply,result.c_str(),result.length()));return reply;
}
int main(){
  config.clear();config["device"]["name"]="original";config["device"]["language"]="zh-CN";
  config["mqtt"]["host"]="broker";config["auth"]["password"]="unchanged";config["lan_peers"][0]["peer"]="02c400000001";
  const std::string name="卧室, DAIKIN: [温度] {计量} \"name\" \\ 😀";
  auto result=call(name);assert(result["code"]=="accepted"&&result["data"]["name"]==name);
  assert(config["device"]["name"]==name&&config["device"]["language"]=="zh-CN"&&mqttRestart&&!wifiRestart&&saves==1);
  // String-valued HAP reads escape the exact stored name, not a lossy alias.
  auto quoted=iotcloud::homeJsonQuote<String>(name.c_str());DynamicJsonDocument read(512);
  assert(!deserializeJson(read,quoted)&&read.as<std::string>()==name);
  const auto old=config["device"]["name"].as<std::string>();
  for(const auto &bad:{std::string(),std::string(64,'x'),std::string("a\0b",3),
      std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),std::string("中中中中中中中中中中中中中中中中中中中中中中")}){
    assert(call(bad)["code"]=="invalid_params"&&config["device"]["name"]==old&&saves==1);
  }
  assert(call("extra",true)["code"]=="invalid_params"&&saves==1);
  const std::string prefix="\x83\xa6" "action" "\xa8" "name.set" "\xaa" "request_id" "\xa4" "bad1" "\xa6" "params";
  for(const auto &bad:{prefix+"\x82\xa4" "name" "\xa1" "a" "\xa4" "name" "\xa1" "b",
      prefix+"\x81\xa9"+std::string("name\0evil",9)+"\xa1" "x"}){
    const auto response=execute(String(bad.data(),bad.size()));DynamicJsonDocument rejected(1024);
    assert(!deserializeMsgPack(rejected,response.c_str(),response.length()));
    assert(rejected["code"]=="invalid_params"&&saves==1&&config["device"]["name"]==old);
  }
  saveReady=false;result=call("unsaved");assert(result["code"]=="storage_error"&&config["device"]["name"]==old&&saves==2);
  saveReady=true;result=call(std::string(63,'x'));assert(result["code"]=="accepted"&&saves==3);
  result=call("idempotent",false,"name.set","stable_name_id");assert(result["code"]=="accepted"&&saves==4);
  result=call("idempotent",false,"name.set","stable_name_id");assert(result["code"]=="accepted"&&saves==4);
  result=call("conflict",false,"name.set","stable_name_id");assert(result["code"]=="conflict"&&saves==4);
  DynamicJsonDocument caps(2048);capabilities(caps.to<JsonObject>());bool offered=false;
  for(JsonVariantConst action:caps["actions"].as<JsonArrayConst>())if(action=="name.set")offered=true;
  assert(offered&&caps["name_max_bytes"]==63);
  puts("Production DAIKIN name.set MP: UTF-8/byte bounds, exact persistence, storage rollback, receipt reuse and capability passed");
}
