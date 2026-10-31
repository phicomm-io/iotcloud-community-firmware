#include "hub_home.h"
#include <cassert>
#include <cstdio>
#include <limits>
extern "C" {
#include "../../../common/device_status.h"
}

using Home = iotcloud::HubHome;
static uint32_t now = 50000;
static uint32_t millis(){return now;}
static iotcloud::HubDeviceBus bus;
static Home hubHome(bus);
static bool homePaused = false;

// Run the unmodified production HomeSensor constructor/loop. The host shim
// collects characteristic values and availability; it is not a HAP transport.
struct SpanCharacteristic;
static std::vector<SpanCharacteristic*> characteristics;
struct SpanCharacteristic {
  double value; bool iotcloudAvailable = false;
  explicit SpanCharacteristic(double value=0):value(value){characteristics.push_back(this);}
  virtual ~SpanCharacteristic(){}
  template<class T=int> T getVal()const{return T(value);}
  template<class T> void setVal(T next){value=next;}
  void setRange(double,double,double){}
  unsigned getIID()const{return 1;}
};
struct SpanService {
  std::string type;
  SpanService(const char *type,const char*):type(type){}
  virtual ~SpanService(){}
  virtual void loop(){}
  virtual bool update(){return true;}
};
using boolean=bool;
static void homeAttachName(size_t,unsigned){}
static bool homeNameUpdated(size_t){return false;}
static bool homeRename(size_t){return false;}
#define REQ(name) ((void)0)
#define OPT(name) ((void)0)
namespace Characteristic {
#define HOST_CHARACTERISTIC(name) struct name:SpanCharacteristic {explicit name(double v=0):SpanCharacteristic(v){}};
HOST_CHARACTERISTIC(CurrentTemperature)
HOST_CHARACTERISTIC(CurrentRelativeHumidity)
HOST_CHARACTERISTIC(CarbonDioxideDetected)
HOST_CHARACTERISTIC(CarbonDioxideLevel)
HOST_CHARACTERISTIC(AirQuality)
HOST_CHARACTERISTIC(PM25Density)
HOST_CHARACTERISTIC(PM10Density)
HOST_CHARACTERISTIC(StatusActive)
HOST_CHARACTERISTIC(StatusFault)
#undef HOST_CHARACTERISTIC
}
#include "home-sensor.inc"

static const char *peer="02c400000001";
static std::string encoded(device_status_t &snapshot,device_status_full_t &full){
  ph_writer writer;assert(phw_init(&writer,PH_WIRE_MSGPACK));
  device_status_mp(&writer,&snapshot,&full);
  auto bytes=phw_take(&writer);assert(bytes);
  std::string result(bytes->buf,bytes->len);ph_buf_free(bytes);return result;
}
static DynamicJsonDocument decoded(const std::string &bytes){
  DynamicJsonDocument doc(8192);assert(!deserializeMsgPack(doc,bytes));return doc;
}
static void observe(const std::string &bytes){hubHome.tick(now);hubHome.observe(hubHome.devices[1].base+"/state",bytes);}
static void observe(const JsonDocument &doc){std::string bytes;serializeMsgPack(doc,bytes);observe(bytes);}
static void discovery(){
  DynamicJsonDocument doc(1024);doc["schema"]=2;doc["protocol"]="tmcp";doc["protocol_version"]=2;
  doc["version"]="v2";doc["device_id"]=peer;doc["model"]="m1";doc["variant"]="full";
  doc["firmware_version"]="1.0.0-rc.4";doc["wire_version"]=1;doc["default_wire_format"]="msgpack";
  doc.createNestedArray("wire_formats").add("msgpack");std::string bytes;serializeMsgPack(doc,bytes);
  iotcloud::hubWire.observe("lab/discovery/"+std::string(peer),bytes);
}
int main(){
  DynamicJsonDocument saved(512);auto device=saved.to<JsonArray>().createNestedObject();
  device["mac"]=peer;device["model"]="m1";
  assert(hubHome.configure(saved.as<JsonArrayConst>(),"lab/","02c400000002"));discovery();
  HomeSensor temperature(1,Home::Temperature),humidity(1,Home::Humidity),pm(1,Home::PM25),co2(1,Home::CO2);
  assert(temperature.type=="8A"&&humidity.type=="82");
  device_status_t snapshot={};device_status_full_t full={};
  snapshot.captured=STATUS_IDENTITY|STATUS_NETWORK|STATUS_CONTROL|STATUS_SENSOR|STATUS_OTA|STATUS_DIAGNOSTIC;
  snapshot.model=DEVICE_M1;snapshot.now_ms=now;snapshot.identity.version="1.0.0-rc.4";
  strcpy(snapshot.identity.id,peer);
  auto &sample=snapshot.sensor.m1.sample;sample.received=true;sample.updated_ms=now-1200;
  sample.pm25_status=sample.climate_status=sample.auxiliary_status=true;
  sample.temperature_deci_c=255;sample.humidity_deci_pct=430;sample.pm25_ug_m3=12;
  snapshot.sensor.m1.co2.ready=snapshot.sensor.m1.co2.received=true;
  snapshot.sensor.m1.co2.updated_ms=now-500;snapshot.sensor.m1.co2.co2_ppm=720;
  const auto wire=encoded(snapshot,full);auto status=decoded(wire);
  assert(status["sensor_health"]["climate_valid"]==true&&status["climate_valid"].isNull());
  ph_value root,age;assert(phv_parse(PH_WIRE_MSGPACK,wire.data(),wire.size(),&root)&&phv_get(&root,"age_ms",&age));
  assert(age.kind==PHV_REAL); // Production age_ms is binary64, not an integer tag.
  observe(wire);auto &d=hubHome.devices[1];
  assert(d.reachable(now)&&d.values[Home::Temperature].valid);
  assert(d.values[Home::Temperature].number==25.5&&d.values[Home::Humidity].number==43);
  assert(d.values[Home::PM25].number==12&&d.values[Home::CO2].number==720);
  temperature.loop();humidity.loop();pm.loop();co2.loop();
  assert(temperature.number->iotcloudAvailable&&temperature.number->getVal<double>()==25.5);
  assert(humidity.number->iotcloudAvailable&&humidity.number->getVal<double>()==43);
  assert(pm.number->iotcloudAvailable&&co2.number->iotcloudAvailable);
  assert(temperature.active->getVal()==1&&temperature.fault->getVal()==0);

  for(double invalid: {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),
                      -1.,1200.5,20000.,double(UINT32_MAX),1e100}){
    auto malformed=decoded(wire);malformed["age_ms"]=invalid;observe(malformed);
    temperature.loop();humidity.loop();pm.loop();co2.loop();
    assert(!d.values[Home::Temperature].valid&&!d.values[Home::Humidity].valid&&!d.values[Home::PM25].valid);
    assert(!temperature.number->iotcloudAvailable&&!temperature.active->getVal()&&temperature.fault->getVal()==1);
    assert(d.values[Home::CO2].valid&&co2.number->iotcloudAvailable); // Independent sensor age.
  }
  for(const char *invalid: {"1200","", "true"}){
    auto malformed=decoded(wire);malformed["age_ms"]=invalid;observe(malformed);assert(!d.values[Home::Temperature].valid);
  }
  for(bool invalid: {true,false}){
    auto malformed=decoded(wire);malformed["age_ms"]=invalid;observe(malformed);assert(!d.values[Home::Temperature].valid);
  }
  auto missingAge=decoded(wire);missingAge.remove("age_ms");observe(missingAge);assert(!d.values[Home::Temperature].valid);
  auto integral=decoded(wire);integral["age_ms"]=1200u;observe(integral);assert(d.values[Home::Temperature].valid);
  auto co2Float=decoded(wire);co2Float["scd40"]["age_ms"]=500.0;observe(co2Float);assert(d.values[Home::CO2].valid);
  for(double badAge:{-1.,500.5,20000.,double(UINT32_MAX),std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity()}){
    auto malformed=decoded(wire);malformed["scd40"]["age_ms"]=badAge;observe(malformed);
    assert(d.values[Home::Temperature].valid&&!d.values[Home::CO2].valid);
  }
  auto missing=decoded(wire);missing.remove("sensor_health");missing["climate_valid"]=true;missing["pm25_valid"]=true;
  observe(missing);assert(!d.values[Home::Temperature].valid&&!d.values[Home::PM25].valid);
  auto invalid=decoded(wire);invalid["sensor_health"]["climate_valid"]=false;observe(invalid);
  assert(!d.values[Home::Temperature].valid&&!d.values[Home::Humidity].valid&&d.values[Home::PM25].valid);
  invalid=decoded(wire);invalid["sensor_health"].remove("pm25_valid");observe(invalid);
  assert(d.values[Home::Temperature].valid&&!d.values[Home::PM25].valid);
  invalid=decoded(wire);invalid["sensor_health"]["climate_valid"]=1;observe(invalid);
  assert(!d.values[Home::Temperature].valid&&d.values[Home::PM25].valid);
  invalid=decoded(wire);invalid["sensor_health"]["climate_valid"]="true";observe(invalid);
  assert(!d.values[Home::Temperature].valid&&d.values[Home::PM25].valid);
  invalid=decoded(wire);invalid["sensor_valid"]=1;observe(invalid);assert(!d.values[Home::Temperature].valid);
  invalid=decoded(wire);invalid["sensor_valid"]=false;observe(invalid);assert(!d.values[Home::Temperature].valid);
  auto boundary=decoded(wire);boundary["age_ms"]=19999.0;observe(boundary);temperature.loop();assert(temperature.number->iotcloudAvailable);
  now++;temperature.loop();assert(!temperature.number->iotcloudAvailable); // Exactly 20s, not fresh.
  now=UINT32_MAX-500;snapshot.now_ms=now;sample.updated_ms=now-1200;
  observe(encoded(snapshot,full));temperature.loop();assert(temperature.number->iotcloudAvailable);
  now+=18800;temperature.loop();assert(!temperature.number->iotcloudAvailable); // Clock wrap.
  for(auto p:characteristics)delete p;
  puts("PASS real M1 encoder MP -> HubHome -> production HomeSensor: nested health, float/integer age, strict invalid rejection, independent CO2, exact expiry/wrap");
}
