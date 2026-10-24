#pragma once
#include <math.h>
#include "frames.h"
#include "../../common/indicator.h"

namespace daikin {
enum class LightMode { Off, On, Slow, Fast };
enum class Compare { Greater, GreaterEqual, Less, LessEqual };
enum LightMetric { LightCO2, LightTVOC, LightPM, LightHCHO, LightMetricCount };
struct LightRule {
  float threshold=0;
  Compare compare=Compare::Greater;
  LightMode mode=LightMode::Off;
};
struct LightRules { LightRule rules[2]; unsigned count=0; };
struct LightSettings { LightRules metrics[LightMetricCount]; bool pm10=false; };
inline const char *lightModeName(LightMode mode) {
  switch(mode) {
    case LightMode::On:return "on";
    case LightMode::Slow:return "slow";
    case LightMode::Fast:return "fast";
    default:return "off";
  }
}
inline LightSettings defaultLightSettings() {
  LightSettings settings;
  const float thresholds[]={1000,300,50,0.1f};
  for(unsigned i=0;i<LightMetricCount;i++) {
    auto &rules=settings.metrics[i];rules.count=1;
    rules.rules[0].threshold=thresholds[i];rules.rules[0].mode=LightMode::On;
  }
  return settings;
}
inline bool lightMatches(const LightRule &rule,float value) {
  switch(rule.compare) {
    case Compare::Greater:return value>rule.threshold;
    case Compare::GreaterEqual:return value>=rule.threshold;
    case Compare::Less:return value<rule.threshold;
    case Compare::LessEqual:return value<=rule.threshold;
  }
  return false;
}
inline LightMode lightMode(const LightRules &rules,const Sample &sample,uint32_t now) {
  if(!sample.valid(now)||!isfinite(sample.value))return LightMode::Off;
  const LightRule *selected=nullptr;
  for(unsigned i=0;i<rules.count&&i<2;i++) {
    const auto &rule=rules.rules[i];
    if(lightMatches(rule,sample.value)&&(!selected||rule.threshold>selected->threshold))selected=&rule;
  }
  return selected?selected->mode:LightMode::Off;
}
inline LightMode sharedLightMode(LightMode a,LightMode b) {
  return static_cast<unsigned>(a)>static_cast<unsigned>(b)?a:b;
}
inline bool lightOn(LightMode mode,uint32_t elapsed) {
  return mode==LightMode::On||(mode==LightMode::Fast&&elapsed%300u<150u)||
    (mode==LightMode::Slow&&elapsed%2000u<1000u);
}
struct LightOutput {
  LightMode mode=LightMode::Off;uint32_t since=0;
  bool update(LightMode next,uint32_t now) {
    if(mode!=next){mode=next;since=now;}
    return lightOn(mode,uint32_t(now-since));
  }
};
inline bool systemLightOn(unsigned mode,uint32_t elapsed) {
  return mode==INDICATOR_READY||indicator_on(mode,elapsed);
}
inline const char *systemLightName(unsigned mode) {
  switch(mode) {
    case INDICATOR_UNCONFIGURED:return "unconfigured";
    case INDICATOR_WIFI_DOWN:return "wifi_down";
    case INDICATOR_MQTT_DOWN:return "mqtt_down";
    default:return "ready";
  }
}
}
