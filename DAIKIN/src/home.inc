/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hub_home_budget.h"
#include <esp_heap_caps.h>
// HAP is deliberately opt-in. Only authenticated local UDP state are trusted as
// live state; upstream retained messages cannot confirm a Home control.
static iotcloud::HomeStorage homeStorage;
static bool homeStarted=false,homePaused=false;
static const char *homeError="disabled";
static char homeCode[9]={},homeSetupId[5]={};
static std::string homeConfiguration;
static uint32_t homeNextQuery=0;
static std::array<SpanCharacteristic *,iotcloud::HubHome::Capacity> homeNames={},homeConfiguredNames={};
static bool homeHealthy(){
  if(!homeStarted||homePaused)return false;
  if(homeSpan.iotcloudReady())return true;
  homeSpan.iotcloudStop();homePaused=true;homeError="storage_error";
  for(auto &d:hubHome.devices){d.online=false;d.controls=false;}
  return false;
}
static bool homeEnabled(){return !configRecovery&&hubService.ready()&&config["hub"]["homekit"]["enabled"]==true;}
static std::string homeSignature(){
  std::string s;serializeJson(config["hub"]["devices"],s);
  return hubService.user()+"/"+std::string(config["hub"]["instance"]|"")+"/"+(homeEnabled()?"1/":"0/")+s;
}
static bool homeCodeAllowed(const char *code){
  if(!strcmp(code,"12345678")||!strcmp(code,"87654321"))return false;
  for(unsigned i=1;i<8;i++)if(code[i]!=code[0])return true;
  return false;
}
static bool homeStoreText(nvs_handle handle,const char *key,const char *text){return nvs_set_str(handle,key,text)==ESP_OK&&nvs_commit(handle)==ESP_OK;}
static bool homeAwaitControl(){
  const uint32_t began=millis();
  do{
    auto now=millis();pollLocalControls(now);hubHome.tick(now);pollHubLan(now);
    if(!homeHealthy()){hubHome.finish();return false;}
    if(hubHome.finished())return hubHome.finish();
    delay(1);
  }while(uint32_t(millis()-began)<2500);
  hubHome.finish();return false;
}
static bool homeControl(size_t device,iotcloud::HubHome::Control control,unsigned channel,double value,bool powerUpdated=false,bool power=false){
  hubHome.tick(millis());
  return homeHealthy()&&!otaActive&&hubHome.begin(device,control,channel,value,powerUpdated,power)&&homeAwaitControl();
}
static void homeAttachName(size_t device,uint32_t nextIID){
  // Reserve one IID outside the original graph, then resume its old sequence.
  // Existing paired controllers retain every original AID and IID.
  homeSpan.resetIID(1000);
  homeConfiguredNames[device]=new Characteristic::ConfiguredName(hubHome.devices[device].name.c_str());
  homeConfiguredNames[device]->iotcloudStringLimit=hubHome.devices[device].model=="tc1"?31:63;
  homeSpan.resetIID(nextIID);
}
static bool homeNameUpdated(size_t device){return homeConfiguredNames[device]&&homeConfiguredNames[device]->updated();}
static bool homeRename(size_t device){
  hubHome.tick(millis());
  if(!homeHealthy()||otaActive)return false;
  const auto &d=hubHome.devices[device];const char *name=homeConfiguredNames[device]->getNewString();
  if(!name||!d.nameWritable(millis()))return false;
  if(d.name==name)return true; // No flash write for an unchanged, confirmed name.
  return hubHome.beginName(device,name)&&homeAwaitControl();
}
static void homeSyncNames(){
  for(size_t i=0;i<hubHome.size;++i){
    const auto &d=hubHome.devices[i];
    if(!d.seen)continue;
    if(homeConfiguredNames[i]&&d.nameMax)homeConfiguredNames[i]->iotcloudStringLimit=d.nameMax;
    if(homeNames[i]&&strcmp(homeNames[i]->getString(),d.name.c_str()))homeNames[i]->setString(d.name.c_str(),false);
    if(homeConfiguredNames[i]&&!homeConfiguredNames[i]->updated()&&strcmp(homeConfiguredNames[i]->getString(),d.name.c_str()))homeConfiguredNames[i]->setString(d.name.c_str());
  }
}
// Relay state uses a Switch service; TC1 and S30-V1B do not report an
// appliance-insertion sensor for an OutletInUse characteristic.
struct HomeOutlet:Service::Switch{
  size_t device;unsigned channel;SpanCharacteristic *on;
  HomeOutlet(size_t d,unsigned c):device(d),channel(c){
    on=new Characteristic::On(false);
    auto label=new Characteristic::Name((std::string("Outlet ")+std::to_string(c+1)).c_str());
    if(!c)homeAttachName(d,label->getIID()+1);
  }
  boolean update()override{
    if(!channel&&homeNameUpdated(device))return !on->updated()&&homeRename(device);
    return !on->updated()||homeControl(device,iotcloud::HubHome::Outlet,channel,on->getNewVal());
  }
  void loop()override{
    const auto &d=hubHome.devices[device];bool live=d.controls&&d.reachable(millis())&&!homePaused;
    on->iotcloudAvailable=live;
    if(live&&on->getVal()!=d.outlets[channel])on->setVal(d.outlets[channel]);
  }
};
struct HomeFan:Service::Fan{
  size_t device;SpanCharacteristic *active,*speed;
  HomeFan(size_t d):device(d){active=new Characteristic::Active(0);speed=new Characteristic::RotationSpeed(0);homeAttachName(d,speed->getIID()+1);}
  boolean update()override{
    if(homeNameUpdated(device))return !speed->updated()&&!active->updated()&&homeRename(device);
    if(speed->updated())return homeControl(device,iotcloud::HubHome::FanSpeed,0,std::round(speed->getNewVal<double>()),active->updated(),active->getNewVal());
    return !active->updated()||homeControl(device,iotcloud::HubHome::FanOn,0,active->getNewVal());
  }
  void loop()override{
    const auto &d=hubHome.devices[device];bool live=d.controls&&d.reachable(millis())&&!homePaused;
    active->iotcloudAvailable=speed->iotcloudAvailable=live;
    if(live){if(active->getVal()!=d.fan)active->setVal(d.fan);if(speed->getVal()!=d.speed)speed->setVal(d.speed);}
  }
};
struct HomeLight:Service::LightBulb{
  size_t device;SpanCharacteristic *on,*brightness;
  HomeLight(size_t d):device(d){on=new Characteristic::On(false);brightness=new Characteristic::Brightness(0);}
  boolean update()override{
    if(brightness->updated())return homeControl(device,iotcloud::HubHome::LightBrightness,0,brightness->getNewVal(),on->updated(),on->getNewVal());
    return !on->updated()||homeControl(device,iotcloud::HubHome::LightOn,0,on->getNewVal());
  }
  void loop()override{
    const auto &d=hubHome.devices[device];bool live=d.controls&&d.reachable(millis())&&!homePaused;
    on->iotcloudAvailable=brightness->iotcloudAvailable=live;
    if(live){unsigned pct=(d.brightness*100+127)/255;if(on->getVal()!=d.light)on->setVal(d.light);if(brightness->getVal()!=int(pct))brightness->setVal(pct);}
  }
};
struct HomeSensor:SpanService{
  size_t device;iotcloud::HubHome::Metric metric;SpanCharacteristic *number,*active,*fault,*quality=nullptr;
  HomeSensor(size_t d,iotcloud::HubHome::Metric m):SpanService(m==iotcloud::HubHome::Temperature?"8A":m==iotcloud::HubHome::Humidity?"82":m==iotcloud::HubHome::CO2?"97":"8D","IoTCloudSensor"),device(d),metric(m){
    // This subclass uses the same required/optional definitions as HomeSpan's
    // standard services; register the characteristic metadata explicitly.
    using namespace iotcloud;
    if(m==HubHome::Temperature){REQ(CurrentTemperature);number=new Characteristic::CurrentTemperature();number->setRange(-100,100,0.1);}
    else if(m==HubHome::Humidity){REQ(CurrentRelativeHumidity);number=new Characteristic::CurrentRelativeHumidity();}
    else if(m==HubHome::CO2){REQ(CarbonDioxideDetected);OPT(CarbonDioxideLevel);quality=new Characteristic::CarbonDioxideDetected();number=new Characteristic::CarbonDioxideLevel();}
    else{REQ(AirQuality);OPT(PM25Density);OPT(PM10Density);quality=new Characteristic::AirQuality();number=m==HubHome::PM25?static_cast<SpanCharacteristic *>(new Characteristic::PM25Density()):static_cast<SpanCharacteristic *>(new Characteristic::PM10Density());}
    OPT(StatusActive);OPT(StatusFault);active=new Characteristic::StatusActive(false);fault=new Characteristic::StatusFault(1);
    if(m==HubHome::Temperature){OPT(ConfiguredName);homeAttachName(d,fault->getIID()+1);}
  }
  boolean update()override{return metric!=iotcloud::HubHome::Temperature||!homeNameUpdated(device)||homeRename(device);}
  void loop()override{
    const auto &d=hubHome.devices[device];const auto &v=d.values[metric];bool live=d.reachable(millis())&&v.fresh(millis())&&!homePaused;
    number->iotcloudAvailable=live;if(quality)quality->iotcloudAvailable=live;
    if(active->getVal()!=live)active->setVal(live);if(fault->getVal()!=!live)fault->setVal(!live);
    if(!live)return;
    if(number->getVal<double>()!=v.number)number->setVal(v.number);
    if(quality){unsigned value=metric==iotcloud::HubHome::CO2?unsigned(v.number>=1000):v.number<=15?1:v.number<=35?2:v.number<=55?3:v.number<=150?4:5;if(quality->getVal()!=int(value))quality->setVal(value);}
  }
};
static void homeAccessory(uint32_t aid,const char *model,const char *serial,const char *name,size_t device=iotcloud::HubHome::Capacity){
  new SpanAccessory(aid);new Service::AccessoryInformation();new Characteristic::Identify();auto label=new Characteristic::Name(name);new Characteristic::Manufacturer("IoTCloud");new Characteristic::Model(model);new Characteristic::SerialNumber(serial);new Characteristic::FirmwareRevision("1.0.0");
  if(device<homeNames.size())homeNames[device]=label;
}
static bool homeMemoryReady(){
  using Budget=iotcloud::HomeGraphBudget;
  auto saved=config["hub"]["devices"].as<JsonArrayConst>();
  const char *owner=config["hub"]["username"]|"";
  const size_t ownerLength=strlen(owner);
  if(saved.isNull()||saved.size()>iotcloud::HubService::Devices||ownerLength<3||ownerLength>32)return false;
  for(size_t i=0;i<ownerLength;++i)if(!((owner[i]>='a'&&owner[i]<='z')||(owner[i]>='A'&&owner[i]<='Z')||(owner[i]>='0'&&owner[i]<='9')||owner[i]=='_'))return false;
  Budget budget({sizeof(SpanAccessory),sizeof(SpanCharacteristic),sizeof(Service::AccessoryInformation),
      sizeof(HomeSensor),sizeof(HomeOutlet),sizeof(HomeFan),sizeof(HomeLight),sizeof(SpanService *)});
  if(!budget.device(Budget::Daikin))return false;
  const char *seen[iotcloud::HubService::Devices]={};size_t count=0;
  for(JsonObjectConst device:saved){
    const char *mac=device["mac"]|"";
    auto model=Budget::kind(device["model"]|"");
    if(model==Budget::Invalid||strlen(mac)!=12||!strcmp(mac,"000000000000")||!strcmp(mac,"ffffffffffff")||
        device["version"]!="v2"||device["status"]!="active"||device["username"]!=owner)return false;
    for(unsigned i=0;i<12;++i)if(!((mac[i]>='0'&&mac[i]<='9')||(mac[i]>='a'&&mac[i]<='f')))return false;
    for(size_t i=0;i<count;++i)if(!strcmp(mac,seen[i]))return false;
    seen[count++]=mac;
    if(strcmp(mac,deviceId.c_str())&&!budget.device(model))return false;
  }
  const auto caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
  return budget.fits(heap_caps_get_free_size(caps),heap_caps_get_largest_free_block(caps));
}
static void homeInit(){
  homeConfiguration=homeSignature();
  if(!homeEnabled())return;
  // Reject the entire graph before NVS/bootstrap or the first Span allocation.
  // These reserves are startup policy, not a measured SRP peak or a guarantee
  // against concurrent Wi-Fi allocations after the check.
  homeError="busy";
  if(!homeMemoryReady())return;
  homeError="storage_error";
  if(!homeStorage.open(true))return;
  nvs_handle metadata=0;
  if(nvs_open_from_partition(homeStorage.partition.label,"IOTCLOUD",NVS_READWRITE,&metadata)!=ESP_OK)return;
  char instance[33]={};size_t length=sizeof(instance);auto result=nvs_get_str(metadata,"instance",instance,&length);
  const char *owner=config["hub"]["instance"]|"";
  if(strlen(owner)!=32){homeError="identity_changed";nvs_close(metadata);return;}
  if(result==ESP_ERR_NVS_NOT_FOUND){if(!homeStoreText(metadata,"instance",owner)){nvs_close(metadata);return;}}
  else if(result!=ESP_OK||strcmp(instance,owner)){homeError="identity_changed";nvs_close(metadata);return;}
  length=sizeof(homeCode);result=nvs_get_str(metadata,"code",homeCode,&length);
  if(result==ESP_ERR_NVS_NOT_FOUND){
    do{snprintf(homeCode,sizeof(homeCode),"%08u",esp_random()%100000000u);}while(!homeCodeAllowed(homeCode));
    if(!homeStoreText(metadata,"code",homeCode)){nvs_close(metadata);return;}
  }else if(result!=ESP_OK||strlen(homeCode)!=8||strspn(homeCode,"0123456789")!=8||!homeCodeAllowed(homeCode)){nvs_close(metadata);return;}
  length=sizeof(homeSetupId);result=nvs_get_str(metadata,"setup_id",homeSetupId,&length);
  if(result==ESP_ERR_NVS_NOT_FOUND){auto id=hubRandom(2);strlcpy(homeSetupId,id.c_str(),sizeof(homeSetupId));if(!homeStoreText(metadata,"setup_id",homeSetupId)){nvs_close(metadata);return;}}
  else if(result!=ESP_OK||strlen(homeSetupId)!=4||strspn(homeSetupId,"0123456789abcdef")!=4){nvs_close(metadata);return;}
  nvs_close(metadata);
  if(!homeSpan.iotcloudStorage(homeStorage.partition.label))return;
  homeSpan.setLogLevel(-1).setPortNum(51826).setHostNameSuffix(deviceId.substring(6).c_str()).setQRID(homeSetupId);
  homeSpan.begin(Category::Bridges,"IoTCloud Hub",deviceHostname.substring(0,deviceHostname.length()-6).c_str(),"IoTCloud Hub");
  if(!homeSpan.iotcloudStorageHealthy()){homeSpan.iotcloudStop();return;}
  // Generate the SRP verifier once. Fail closed if its atomic NVS write failed.
  nvs_handle srp=0;if(nvs_open_from_partition(homeStorage.partition.label,"SRP",NVS_READWRITE,&srp)!=ESP_OK){homeSpan.iotcloudStop();return;}
  length=0;result=nvs_get_blob(srp,"VERIFYDATA",nullptr,&length);
  if(result==ESP_ERR_NVS_NOT_FOUND)homeSpan.setPairingCode(homeCode,false);
  bool verified=nvs_get_blob(srp,"VERIFYDATA",nullptr,&length)==ESP_OK&&length==sizeof(Verification);
  nvs_close(srp);if(!verified||!homeSpan.iotcloudStorageHealthy()){homeSpan.iotcloudStop();return;}
  if(!hubHome.configure(config["hub"]["devices"],hubService.prefix(),deviceId.c_str())){homeError="invalid_params";homeSpan.iotcloudStop();return;}
  homeAccessory(1,"IoTCloud Hub",deviceId.c_str(),"IoTCloud Hub");
  using Home=iotcloud::HubHome;
  for(size_t i=0;i<hubHome.size;i++){
    const auto &d=hubHome.devices[i];homeAccessory(d.aid,d.model.c_str(),d.mac.c_str(),d.name.c_str(),i);
    if(d.outletCount())for(unsigned c=0;c<d.outletCount();c++)new HomeOutlet(i,c);
    else if(d.model=="a1"){new HomeFan(i);new HomeLight(i);}
    else{new HomeSensor(i,Home::Temperature);new HomeSensor(i,Home::Humidity);new HomeSensor(i,Home::CO2);new HomeSensor(i,Home::PM25);if(d.model=="daikin")new HomeSensor(i,Home::PM10);}
  }
  if(!homeSpan.iotcloudInitialize()){homeSpan.iotcloudStop();return;}
  homeStarted=true;homeError="ok";
}
static void homeStatus(JsonObject out){
  bool ready=homeHealthy();
  out["ready"]=ready;out["code"]=homeError;out["paired"]=ready&&HAPClient::nAdminControllers()>0;out["devices"]=homeStarted?hubHome.size:0;out["port"]=51826;
  if(ready&&!HAPClient::nAdminControllers()){
    char pin[11];snprintf(pin,sizeof(pin),"%.3s-%.2s-%.3s",homeCode,homeCode+3,homeCode+5);out["setup_code"]=pin;
    HapQR qr;out["setup_uri"]=qr.get(atoi(homeCode),homeSetupId,unsigned(Category::Bridges));
  }
}
static bool homeReset(){
  // Erase failure can still leave a partially erased pairing store.
  homeSpan.iotcloudStop();homePaused=true;homeError="storage_error";
  if(!homeStorage.reset())return false;
  homeError="restart_required";rebootAt=millis()+1500;return true;
}
static bool homeFactoryReset(){
  if(homeStorage.open(false)||homeStorage.owned)return homeReset();
  // The ordinary reset may proceed, but unidentified tail data is untouched.
  return true;
}
static void homeQuiesce(){
  homePaused=true;homeError="restart_required";
  if(homeStarted)homeSpan.iotcloudStop();
  // Restoring even the same device list revokes the old owner's live session.
  for(auto &d:hubHome.devices){d.online=false;d.controls=false;}
  hubHome.finish();
}
static void homeChanged(){
  if(homeConfiguration!=homeSignature()){
    homeQuiesce();
    if(homeStarted||homeEnabled())rebootAt=millis()+1500;
  }
}
static void pollHome(uint32_t now){
  hubHome.tick(now);
  if(!homeHealthy()||otaActive)return;
  if(due(now,homeNextQuery)){homeNextQuery=now+1000;hubHome.refresh();}
  homeSyncNames();
  homeSpan.poll();
  homeHealthy();
}
