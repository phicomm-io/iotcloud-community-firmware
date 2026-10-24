#pragma once
#include <ArduinoJson.h>
#include <string.h>
#include "indicators.h"

namespace daikin {
static const char *const lightMetricNames[]={"co2","tvoc","pm","formaldehyde"};
static const char *const lightCompareNames[]={"gt","gte","lt","lte"};
inline bool lightFields(JsonObjectConst object,const char *const *allowed,unsigned count) {
  for(JsonPairConst pair:object) {
    bool found=false;
    for(unsigned i=0;i<count;i++)if(!strcmp(pair.key().c_str(),allowed[i]))found=true;
    if(!found)return false;
  }
  return true;
}
// A full replacement is validated before touching either RAM or NVS.
inline bool readLightSettings(JsonVariantConst value,LightSettings &out) {
  if(!value.is<JsonObjectConst>())return false;
  JsonObjectConst object=value.as<JsonObjectConst>();
  const char *const allowed[]={"co2","tvoc","pm","formaldehyde","pm_source"};
  if(!lightFields(object,allowed,5)||!object["pm_source"].is<const char*>())return false;
  const char *pmSource=object["pm_source"];
  if(strcmp(pmSource,"pm25")&&strcmp(pmSource,"pm10"))return false;
  LightSettings candidate;
  candidate.pm10=!strcmp(pmSource,"pm10");
  const float maxima[]={10000,65535,10000,5};
  const char *const fields[]={"threshold","op","mode"};
  for(unsigned metric=0;metric<LightMetricCount;metric++) {
    if(metric==LightHCHO&&!object.containsKey("formaldehyde"))continue;
    auto list=object[lightMetricNames[metric]];
    if(!list.is<JsonArrayConst>()||list.size()>2)return false;
    auto &rules=candidate.metrics[metric];
    for(JsonVariantConst item:list.as<JsonArrayConst>()) {
      if(!item.is<JsonObjectConst>()||item.size()!=3||!lightFields(item.as<JsonObjectConst>(),fields,3)||
         !item["threshold"].is<float>()||!item["op"].is<const char*>()||!item["mode"].is<const char*>())return false;
      LightRule rule;rule.threshold=item["threshold"].as<float>();
      if(!isfinite(rule.threshold)||rule.threshold<0||rule.threshold>maxima[metric])return false;
      if(rules.count&&rules.rules[0].threshold==rule.threshold)return false;
      bool found=false;
      for(unsigned i=0;i<4;i++)if(!strcmp(item["op"],lightCompareNames[i])){rule.compare=static_cast<Compare>(i);found=true;}
      if(!found)return false;
      found=false;
      for(unsigned i=0;i<4;i++)if(!strcmp(item["mode"],lightModeName(static_cast<LightMode>(i)))){rule.mode=static_cast<LightMode>(i);found=true;}
      if(!found)return false;
      rules.rules[rules.count++]=rule;
    }
  }
  out=candidate;return true;
}
inline void writeLightSettings(JsonObject object,const LightSettings &settings) {
  object["pm_source"]=settings.pm10?"pm10":"pm25";
  for(unsigned i=0;i<LightMetricCount;i++) {
    JsonArray list=object.createNestedArray(lightMetricNames[i]);
    const auto &rules=settings.metrics[i];
    for(unsigned j=0;j<rules.count;j++) {
      const auto &rule=rules.rules[j];JsonObject item=list.createNestedObject();
      item["threshold"]=rule.threshold;item["op"]=lightCompareNames[static_cast<unsigned>(rule.compare)];
      item["mode"]=lightModeName(rule.mode);
    }
  }
}
}
