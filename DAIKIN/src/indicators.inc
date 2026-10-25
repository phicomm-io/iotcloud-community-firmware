static unsigned systemIndicatorMode(){
#ifdef IOTCLOUD_HUB
  return indicator_mode(strlen(config["wifi"]["ssid"]|"")>0,WiFi.status()==WL_CONNECTED,hubService.ready(),hubReady());
#else
  return indicator_mode(strlen(config["wifi"]["ssid"]|"")>0,WiFi.status()==WL_CONNECTED,
    strlen(config["mqtt"]["host"]|"")>0,mqttConnected&&mqttCommandReady);
#endif
}
static void sensorIndicatorModes(uint32_t now,daikin::LightMode (&modes)[3]){
  using namespace daikin;
  const unsigned indices[]={CO2,TVOC,lightSettings.pm10?PM10:PM25,HCHO};
  LightMode values[LightMetricCount];
  for(unsigned i=0;i<LightMetricCount;i++)values[i]=indicator?lightMode(lightSettings.metrics[i],readings[indices[i]],now):LightMode::Off;
  modes[0]=values[LightCO2];
  // Existing installations keep their automatic HCHO/TVOC selection until
  // rules are explicitly saved. Custom rules can use both on the shared lamp.
  modes[1]=customLightSettings?sharedLightMode(values[LightTVOC],values[LightHCHO]):hchoPresent?values[LightHCHO]:values[LightTVOC];
  modes[2]=values[LightPM];
}
static void pollIndicators(uint32_t now){
  static daikin::LightOutput outputs[3];
  static unsigned previousSystem=INDICATOR_UNCONFIGURED;
  static uint32_t systemSince=0;
  const unsigned pins[]={4,32,33};daikin::LightMode modes[3];sensorIndicatorModes(now,modes);
  for(unsigned i=0;i<3;i++)digitalWrite(pins[i],outputs[i].update(modes[i],now));
  unsigned mode=systemIndicatorMode();
  if(mode!=previousSystem){previousSystem=mode;systemSince=now;}
  digitalWrite(25,daikin::systemLightOn(mode,uint32_t(now-systemSince)));
}
static void indicatorStatus(JsonObject out,uint32_t now){
  out["indicator_rules_supported"]=true;
  out["indicator_rules_custom"]=customLightSettings;
  daikin::writeLightSettings(out.createNestedObject("indicator_rules"),lightSettings);
  daikin::LightMode modes[3];sensorIndicatorModes(now,modes);
  JsonObject lights=out.createNestedObject("indicator_modes");
  const char *names[]={"co2","tvoc","pm"};
  for(unsigned i=0;i<3;i++)lights[names[i]]=daikin::lightModeName(modes[i]);
  out["system_indicator"]=daikin::systemLightName(systemIndicatorMode());
}
