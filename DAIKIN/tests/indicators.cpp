#include "indicator_json.h"
#include <assert.h>
#include <stdio.h>
#include <string>

static daikin::LightSettings decode(const char *text){
  DynamicJsonDocument doc(8192);assert(!deserializeJson(doc,text));
  daikin::LightSettings out;assert(daikin::readLightSettings(doc.as<JsonVariantConst>(),out));return out;
}
int main(){
  using namespace daikin;
  auto settings=decode(R"({"co2":[{"threshold":1500,"op":"gte","mode":"fast"},{"threshold":1000,"op":"gt","mode":"slow"}],"tvoc":[{"threshold":300,"op":"gt","mode":"on"}],"pm_source":"pm10","pm":[{"threshold":120,"op":"gte","mode":"slow"},{"threshold":200,"op":"gte","mode":"fast"}],"formaldehyde":[{"threshold":0.1,"op":"gt","mode":"on"}]})");
  assert(settings.pm10&&settings.metrics[LightPM].count==2);
  Sample sample;sample.set(1000,10);
  assert(lightMode(settings.metrics[LightCO2],sample,11)==LightMode::Off);
  sample.value=1001;assert(lightMode(settings.metrics[LightCO2],sample,11)==LightMode::Slow);
  sample.value=1500;assert(lightMode(settings.metrics[LightCO2],sample,11)==LightMode::Fast);
  assert(lightMode(settings.metrics[LightCO2],sample,20010)==LightMode::Off);
  sample.set(1500,UINT32_MAX-10);assert(lightMode(settings.metrics[LightCO2],sample,10)==LightMode::Fast);
  sample.value=NAN;assert(lightMode(settings.metrics[LightCO2],sample,11)==LightMode::Off);
  LightRule rule;rule.threshold=50;
  for(unsigned op=0;op<4;op++){
    rule.compare=static_cast<Compare>(op);
    assert(lightMatches(rule,49)==(op==2||op==3));
    assert(lightMatches(rule,50)==(op==1||op==3));
    assert(lightMatches(rule,51)==(op==0||op==1));
  }
  // A higher threshold can explicitly turn off a lower-threshold blinking rule.
  settings.metrics[LightCO2].rules[0].mode=LightMode::Off;
  sample.set(2000,0);assert(lightMode(settings.metrics[LightCO2],sample,0)==LightMode::Off);
  // Shared lamps select a pattern, never OR two instantaneous blink levels.
  for(unsigned a=0;a<4;a++)for(unsigned b=0;b<4;b++)
    assert(static_cast<unsigned>(sharedLightMode(static_cast<LightMode>(a),static_cast<LightMode>(b)))==(a>b?a:b));
  for(unsigned ms=0;ms<12000;ms++){
    assert(!lightOn(LightMode::Off,ms));assert(lightOn(LightMode::On,ms));
    assert(lightOn(LightMode::Fast,ms)==(ms%300<150));
    assert(lightOn(LightMode::Slow,ms)==(ms%2000<1000));
    assert(systemLightOn(INDICATOR_READY,ms));
    assert(systemLightOn(INDICATOR_WIFI_DOWN,ms)==(ms%300<150));
    assert(systemLightOn(INDICATOR_MQTT_DOWN,ms)==(ms%2000<1000));
    unsigned phase=ms%6000;bool pulse=false;
    const unsigned starts[]={0,1000,2000,3000,3400,3800},lengths[]={800,800,800,200,200,200};
    for(unsigned i=0;i<6;i++)if(phase>=starts[i]&&phase<starts[i]+lengths[i])pulse=true;
    assert(systemLightOn(INDICATOR_UNCONFIGURED,ms)==pulse);
  }
  LightOutput output;
  assert(output.update(LightMode::Fast,UINT32_MAX-99));
  assert(output.update(LightMode::Fast,49));assert(!output.update(LightMode::Fast,50));
  assert(output.update(LightMode::Slow,51));assert(!output.update(LightMode::Off,52));
  for(unsigned bits=0;bits<16;bits++)assert(indicator_mode(bits&8,bits&4,bits&2,bits&1)==
    (!(bits&8)?INDICATOR_UNCONFIGURED:!(bits&4)?INDICATOR_WIFI_DOWN:(bits&2)&&!(bits&1)?INDICATOR_MQTT_DOWN:INDICATOR_READY));
  auto defaults=defaultLightSettings();sample.set(1001,0);
  assert(lightMode(defaults.metrics[LightCO2],sample,0)==LightMode::On);
  assert(!defaults.pm10&&defaults.metrics[LightPM].count==1);
  // Exercise the production JSON codec as used on NVS save/reboot.
  DynamicJsonDocument doc(8192);writeLightSettings(doc.to<JsonObject>(),settings);
  std::string persisted;serializeJson(doc,persisted);
  auto restored=decode(persisted.c_str());
  assert(restored.pm10);
  DynamicJsonDocument roundtrip(8192);writeLightSettings(roundtrip.to<JsonObject>(),restored);
  std::string again;serializeJson(roundtrip,again);assert(again==persisted);
  assert(restored.metrics[LightHCHO].rules[0].threshold==.1f);
  for(const char *invalid:{"null","[]","{}",R"({"co2":[],"tvoc":[],"pm":[],"pm_source":"pm25","extra":[]})"}){
    doc.clear();assert(!deserializeJson(doc,invalid));assert(!readLightSettings(doc.as<JsonVariantConst>(),restored));
  }
  const std::string base=R"({"co2":[],"tvoc":[],"pm":[],"pm_source":"pm25"})";
  const char *badRules[]={
    R"([{"threshold":1,"op":"gt","mode":"on"},{"threshold":2,"op":"gt","mode":"slow"},{"threshold":3,"op":"gt","mode":"fast"}])",
    R"([{"threshold":1,"op":"gt","mode":"on"},{"threshold":1,"op":"lt","mode":"off"}])",
    R"([{"threshold":-1,"op":"gt","mode":"on"}])",R"([{"threshold":10001,"op":"gt","mode":"on"}])",
    R"([{"threshold":"1000","op":"gt","mode":"on"}])",R"([{"threshold":true,"op":"gt","mode":"on"}])",
    R"([{"threshold":null,"op":"gt","mode":"on"}])",R"([{"threshold":1e100,"op":"gt","mode":"on"}])",
    R"([{"threshold":1,"op":"eq","mode":"on"}])",R"([{"threshold":1,"op":"gt","mode":"rainbow"}])",
    R"([{"threshold":1,"op":"gt"}])",R"([{"threshold":1,"op":"gt","mode":"on","extra":0}])",
    "[true]","{}","null"
  };
  for(const char *bad:badRules){
    doc.clear();assert(!deserializeJson(doc,base));
    DynamicJsonDocument array(2048);assert(!deserializeJson(array,bad));doc["co2"].set(array.as<JsonVariantConst>());
    assert(!readLightSettings(doc.as<JsonVariantConst>(),restored));
    assert(restored.metrics[LightHCHO].rules[0].threshold==.1f); // Rejection is atomic.
  }
  for(const char *bad:{"pm1","both","", "PM10"}){
    doc.clear();assert(!deserializeJson(doc,base));doc["pm_source"]=bad;
    assert(!readLightSettings(doc.as<JsonVariantConst>(),restored));assert(restored.pm10);
  }
  auto disabled=decode(base.c_str());for(const auto &rules:disabled.metrics)assert(rules.count==0);
  puts("PASS indicator rule bounds, precedence, four patterns, shared lamps, stale data, system signals, rollover and persistence codec");
}
