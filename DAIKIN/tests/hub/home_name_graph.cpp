// Execute the production HomeKit constructors, update callbacks and name loop.
// Only HomeSpan/SDK I/O is replaced; the HubHome queue and MP receipts are real.
#include "hub_home.h"
#include <cassert>
#include <cstdio>
#include <deque>
#include <set>
using boolean=bool;
using Home=iotcloud::HubHome;
static iotcloud::HubDeviceBus bus;
static Home hubHome(bus);
static uint32_t clockMs=10001,currentAid=0,currentIID=0;
static bool homePaused=false,otaActive=false,healthy=true;
static uint32_t millis(){return clockMs;}
static void delay(unsigned n){clockMs+=n;}
static bool homeHealthy(){return healthy&&!homePaused;}
struct SpanService;
static SpanService *currentService;
static std::vector<std::pair<uint32_t,uint32_t>> identities;
struct SpanCharacteristic {
  SpanService *owner;uint32_t iid;double value=0,next=0;
  std::string text,nextText;bool flag=false,iotcloudAvailable=true;
  uint8_t iotcloudStringLimit=0;
  explicit SpanCharacteristic(double v=0):owner(currentService),iid(++currentIID),value(v),next(v){identities.emplace_back(currentAid,iid);}
  explicit SpanCharacteristic(const char *v):SpanCharacteristic(){text=nextText=v;}
  virtual ~SpanCharacteristic(){}
  uint32_t getIID(){return iid;}
  bool updated(){return flag;}
  template<class T=int>T getVal(){return T(value);}
  template<class T=int>T getNewVal(){return T(next);}
  template<class T>void setVal(T v){value=next=v;}
  char *getString(){return const_cast<char *>(text.c_str());}
  char *getNewString(){return const_cast<char *>(nextText.c_str());}
  void setString(const char *v,bool=true){assert(!flag);text=nextText=v;}
  void setRange(double,double,double){}
};
static std::vector<SpanCharacteristic*> chars;
static std::vector<SpanService*> services;
struct SpanService {
  std::string type;
  SpanService(const char *type,const char * = ""):type(type){currentService=this;identities.emplace_back(currentAid,++currentIID);services.push_back(this);}
  virtual ~SpanService(){}
  virtual bool update(){return true;}
  virtual void loop(){}
};
struct SpanAccessory;
static std::vector<SpanAccessory*> accessories;
struct SpanAccessory {explicit SpanAccessory(uint32_t aid){currentAid=aid;currentIID=0;accessories.push_back(this);}};
struct {void resetIID(uint32_t iid){currentIID=iid-1;}}homeSpan;
namespace Service {
struct AccessoryInformation:SpanService{AccessoryInformation():SpanService("3E") {}};
struct Switch:SpanService{Switch():SpanService("49") {}};
struct Fan:SpanService{Fan():SpanService("B7") {}};
struct LightBulb:SpanService{LightBulb():SpanService("43") {}};
}
namespace Characteristic {
#define HOST_NUM(name) struct name:SpanCharacteristic{explicit name(double n=0):SpanCharacteristic(n){chars.push_back(this);}};
#define HOST_TEXT(name) struct name:SpanCharacteristic{explicit name(const char *v):SpanCharacteristic(v){chars.push_back(this);}};
HOST_NUM(Identify) HOST_NUM(On) HOST_NUM(Active) HOST_NUM(RotationSpeed) HOST_NUM(Brightness)
HOST_NUM(CurrentTemperature) HOST_NUM(CurrentRelativeHumidity) HOST_NUM(CarbonDioxideDetected)
HOST_NUM(CarbonDioxideLevel) HOST_NUM(AirQuality) HOST_NUM(PM25Density) HOST_NUM(PM10Density)
HOST_NUM(StatusActive) HOST_NUM(StatusFault)
HOST_TEXT(Name) HOST_TEXT(ConfiguredName) HOST_TEXT(Manufacturer) HOST_TEXT(Model)
HOST_TEXT(SerialNumber) HOST_TEXT(FirmwareRevision)
#undef HOST_NUM
#undef HOST_TEXT
}
#define REQ(name) ((void)0)
#define OPT(name) ((void)0)
static std::array<SpanCharacteristic *,Home::Capacity> homeNames={},homeConfiguredNames={};
static std::deque<std::pair<std::string,std::string>> events;
static void pollLocalControls(uint32_t){
  if(events.empty())return;
  auto event=events.front();events.pop_front();hubHome.observe(event.first,event.second);
}
static void pollHubLan(uint32_t){}
#include "home-name-graph.inc"
static std::string wire(const JsonDocument &doc){std::string out;serializeMsgPack(doc,out);return out;}
static DynamicJsonDocument status(size_t i,const std::string &name){
  DynamicJsonDocument doc(2048);const auto &d=hubHome.devices[i];
  doc["model"]=d.model;doc["device_id"]=d.mac;doc["name"]=name;
  if(d.outletCount()){auto a=doc.createNestedArray("socket_states");for(unsigned c=0;c<d.outletCount();++c){if(d.model=="tc1")a.add(0);else a.add(false);}}
  else if(d.model=="a1"){doc["fan_on"]=false;doc["percentage"]=0;doc["led_visible"]=false;doc["brightness"]=0;}
  else if(d.model=="m1")doc["sensor_valid"]=false;
  else doc.createNestedObject("sensors");
  return doc;
}
static bool writeName(size_t i,const std::string &name){
  auto c=homeConfiguredNames[i];const auto old=c->text;
  c->nextText=name;c->flag=true;const bool ok=c->owner->update();
  c->flag=false;if(ok)c->text=c->nextText;else c->nextText=old;
  homeSyncNames();return ok;
}
int main(){
  DynamicJsonDocument saved(1024);auto a=saved.to<JsonArray>();unsigned mac=1;
  for(const char *model:{"tc1","m1","a1","s30-v1b"}){auto d=a.createNestedObject();char id[13];snprintf(id,sizeof(id),"02c4000000%02x",mac++);d["mac"]=id;d["model"]=model;}
  assert(hubHome.configure(saved.as<JsonArrayConst>(),"lab/","02c400000099"));
  struct DeviceId{const char *c_str(){return "02c400000099";}}deviceId;
  #include "home-graph-build.inc"
  const auto stable=identities;
  for(size_t i=0;i<hubHome.size;++i){
    auto &d=hubHome.devices[i];const unsigned end=d.model=="daikin"?30:d.model=="tc1"?25:d.model=="m1"?25:d.model=="a1"?13:10;
    DynamicJsonDocument discovery(1024);discovery["schema"]=2;discovery["protocol"]="tmcp";discovery["protocol_version"]=2;
    discovery["version"]="v2";discovery["device_id"]=d.mac;discovery["model"]=d.model;discovery["variant"]="full";
    discovery["firmware_version"]="test";discovery["wire_version"]=1;discovery["default_wire_format"]="msgpack";
    discovery.createNestedArray("wire_formats").add("msgpack");iotcloud::hubWire.observe("lab/discovery/"+d.mac,wire(discovery));
    std::set<unsigned> ids;for(const auto &identity:identities)if(identity.first==d.aid)ids.insert(identity.second);
    assert(ids.size()==end+1&&ids.count(1000));for(unsigned iid=1;iid<=end;++iid)assert(ids.count(iid));
    auto name=homeConfiguredNames[i];assert(name&&name->getIID()==1000);
    assert(name->owner->type==(d.outletCount()?"49":d.model=="a1"?"B7":"8A"));
    d.nameMax=d.model=="tc1"?31:63;d.nameCapabilitiesAt=clockMs;d.nameCapabilitiesKnown=true;
    hubHome.tick(clockMs);hubHome.observe(d.base+"/state",wire(status(i,"original")));
    homeSyncNames();assert(homeNames[i]->text=="original"&&name->text=="original");
  }
  unsigned serial=0,writes=0;bool savedOk=true,admit=true,respond=true;
  std::array<std::string,Home::Capacity> savedNames;for(auto &name:savedNames)name="original";
  hubHome.randomHex=[&](size_t n){auto s=std::to_string(++serial);return std::string(2*n-s.size(),'0')+s;};
  bus.dispatch=[&](const std::string &route,const std::string &body,bool,iotcloud::HubDeviceBus::Origin){
    size_t i=0;while(i<hubHome.size&&route!=hubHome.devices[i].base+"/command")++i;assert(i<hubHome.size);
    DynamicJsonDocument cmd(1024),reply(2048);assert(!deserializeMsgPack(cmd,body));
    reply["request_id"]=cmd["request_id"];
    if(cmd["action"]=="name.set"){
      ++writes;if(!admit)return false;
      if(savedOk)savedNames[i]=cmd["params"]["name"].as<std::string>();
      reply["code"]=savedOk?"accepted":"storage_error";
    }else{assert(cmd["action"]=="status.get");reply["code"]="ok";reply["data"].set(status(i,savedNames[i]));}
    if(respond)events.emplace_back(hubHome.devices[i].base+"/result",wire(reply));
    return true;
  };
  for(size_t i=0;i<hubHome.size;++i){assert(writeName(i,"同名, [温度]: \"name\" \\"));assert(homeNames[i]->text==savedNames[i]&&homeConfiguredNames[i]->text==savedNames[i]);}
  const auto before=homeConfiguredNames[2]->text;
  savedOk=false;assert(!writeName(2,"failed")&&homeConfiguredNames[2]->text==before);savedOk=true;
  admit=false;assert(!writeName(2,"busy")&&homeNames[2]->text==before);admit=true;
  hubHome.devices[2].nameMax=0;const auto count=writes;
  assert(!writeName(2,"unsupported")&&writes==count);hubHome.devices[2].nameMax=63;
  assert(!writeName(1,std::string(32,'x'))&&writes==count);assert(!writeName(2,"")&&writes==count);
  // An external device rename updates both exposed characteristics, including
  // an offline cached identity, without rebuilding or changing pairing IDs.
  hubHome.observe(hubHome.devices[2].base+"/state",wire(status(2,"设备侧改名")));homeSyncNames();
  assert(homeNames[2]->text=="设备侧改名"&&homeConfiguredNames[2]->text=="设备侧改名"&&identities==stable);
  // A name and relay update in one service are rejected before either command.
  HomeOutlet *outlet=nullptr;
  for(auto service:services)if((outlet=dynamic_cast<HomeOutlet *>(service))&&outlet->device==1&&outlet->channel==0)break;
  assert(outlet);outlet->on->flag=true;assert(!writeName(1,"mixed")&&writes==count);outlet->on->flag=false;
  assert(writeName(2,"设备侧改名")&&writes==count); // unchanged name has no flash write
  healthy=false;assert(!writeName(2,"storage unavailable")&&writes==count);healthy=true;
  for(auto c:chars)delete c;for(auto s:services)delete s;
  for(auto a:accessories)delete a;
  puts("Production HomeKit name graph: legal primary services, stable AID/IIDs, bidirectional callback, atomic failure and no control side effect passed");
}
