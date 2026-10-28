#include "../../common/i18n.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include "wire_mp.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Preferences.h>
#include <nvs.h>
#include <memory>
#include <atomic>
#include <freertos/semphr.h>
extern "C" {
#include "../../common/local_auth.h"
#include "../../common/typed_wire.h"
#include "../../common/lan_wire.h"
#include "../../common/lan_wire_v4.h"
}
#include <ESPmDNS.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <mqtt_client.h>
#include <mbedtls/base64.h>
#ifdef IOTCLOUD_HUB
#include <esp_heap_caps.h>
#include <mbedtls/pkcs5.h>
#include "hub_service.h"
#include "hub_native_publish.h"
#include "hub_device_bus.h"
#include "hub_lan_discovery.h"
#include "hub_lan_client.h"
#include "hub_lan_result.h"
#include <mdns.h>
#include "hub_relay.h"
#include "hub_home.h"
#include "hub_home_storage.h"
#include <HomeSpan.h>
#include <HAP.h>
#include "hub_web.h"
#endif
#include "frames.h"
#include "cm1106.h"
#include "climate.h"
#include "indicator_json.h"
extern "C" {
#include "../../common/ota/signed_ota.h"
}
#include "web.h"

// BRY88AX151K family, tested BRY88AB151K board wiring; see README.
static TwoWire pmBus(0), thBus(1);
static HardwareSerial co2Uart(2), hchoUart(1);
enum Sensor { PM25, CO2, TEMPERATURE, HUMIDITY, TVOC, HCHO, PM10, SENSOR_COUNT };
static daikin::Sample readings[SENSOR_COUNT];
static uint32_t sensorBoot=0,sensorSequence=0;
static const char *keys[]={"pm25","co2","temperature","humidity","tvoc","formaldehyde","pm10"};
static const char *units[]={"µg/m³","ppm","°C","%","ppb","mg/m³","µg/m³"};
static Preferences prefs;
static httpd_handle_t web;
static esp_mqtt_client_handle_t mqtt=nullptr;
static volatile bool mqttConnected=false;
static volatile bool mqttCommandReady=false;
static volatile int mqttLastEvent=MQTT_EVENT_ANY,mqttErrorType=0,mqttSocketError=0;
static bool hchoPresent=false, indicator=false;
static bool sensorPresent(unsigned i){return i==HCHO?hchoPresent:i!=PM10||readings[PM10].received;}
static int commandSubId=-1;
static bool sgpReady=false;
static uint32_t sgpSince=0, nextSample=0, nextSgp=0, nextPublish=0, connectAt=0;
static volatile uint32_t rebootAt=0;
static String deviceId, apName, mqttRoot;
static portMUX_TYPE mqttMux=portMUX_INITIALIZER_UNLOCKED;
static char mqttPending[4097];
static volatile bool mqttQueued=false;
static bool mqttReceiving=false;
static size_t mqttReceived=0;
#ifdef IOTCLOUD_HUB
static constexpr size_t configCapacity=16384;
#else
static constexpr size_t configCapacity=6144;
#endif
static DynamicJsonDocument config(configCapacity);
static bool configRecovery=false;
#ifdef IOTCLOUD_HUB
static iotcloud::HubService hubService(config);
static iotcloud::HubDeviceBus hubDeviceBus;
static iotcloud::HubRelay hubRelay(config,hubService,hubDeviceBus);
static iotcloud::HubHome hubHome(hubDeviceBus);
static bool homeReset();
static bool homeFactoryReset();
static void homeStatus(JsonObject);
static void homeChanged();
static void homeQuiesce();
static void homeInit();
static void pollHome(uint32_t);
static bool hubReady();
static void pollLocalControls(uint32_t);
static bool hubAllocationReady(size_t);
static bool hubLanSend(const std::string &,const std::string &,bool);
static void pollHubLan(uint32_t);
static void hubLanReset();
static void hubLanStatus(JsonObject);
static void hubApiExecute();
static void hubLocalPublish(const String &,const String &,bool);
static bool hubCapture(esp_mqtt_event_handle_t,unsigned);
static void hubSubscribe();
static void hubConfigureUpstream();
static void hubUpstreamChanged();
static void hubRestoreChanged();
static bool hubRestartPending();
#endif
static daikin::LightSettings lightSettings=daikin::defaultLightSettings();
static bool customLightSettings=false;
static String lastRequest[8], lastBody[8], lastResponse[8];
static unsigned requestSlot=0;
static daikin::CM1106 co2Module;
static uint8_t hchoFrame[9];
static size_t hchoUsed=0;
static bool hchoWaiting=false;
static uint32_t hchoSent=0;
static bool mqttRestart=false, wifiRestart=false;
#include "build_info.h"
#include "firmware_identity.h"
static const char *version=DAIKIN_VERSION;
extern "C" {
extern const daikin::FirmwareIdentity daikin_target __attribute__((section(".rodata_custom_desc"),used)) = {
  DAIKIN_TARGET,"PHICOMM-DAIKIN-1:" DAIKIN_VARIANT
};
}
static char webPassword[33]={};
static portMUX_TYPE authMux=portMUX_INITIALIZER_UNLOCKED;
static void updatePassword(){
  const char *pass=config["auth"]["password"]|"";
  portENTER_CRITICAL(&authMux);strlcpy(webPassword,pass,sizeof(webPassword));portEXIT_CRITICAL(&authMux);
}
static const char *pmModel="detecting";

static ph_auth_state localAuth;
static ph_auth_store loadedAuthStore;
static SemaphoreHandle_t localAuthMutex;
static bool localAuthReady=false;
static void invalidateBusinessStatusCache();
static bool pollBusinessStatusCache(uint32_t);
static bool lanBusinessPacket(const ph_value *,const char *,uint16_t);
static void pollLanBusiness();
static void advertiseLanBusiness();
#ifdef IOTCLOUD_HUB
static bool hubLanPacket(const ph_value *,const char *,uint16_t);
static void initHubLan();
#endif
static bool saveConfig(DynamicJsonDocument &);
static bool saveConfigTransaction(DynamicJsonDocument &,bool);
#include "auth_storage.inc"
static String loadLegacyConfigText(){
  const char *key="config";
#ifdef IOTCLOUD_HUB
  // Never reuse the legacy string key for a blob: IDF may retain both types.
  // A present hub key is authoritative, including invalid data (fail closed).
  if(prefs.isKey("config_hub"))key="config_hub";
#endif
  auto type=prefs.getType(key);
  if(type==PT_BLOB){
    const size_t n=prefs.getBytesLength(key);
    if(n&&n<=6144){std::unique_ptr<char[]> data(new(std::nothrow) char[n+1]);if(data&&prefs.getBytes(key,data.get(),n)==n){data[n]=0;return String(data.get(),n);}}
    return "invalid";
  }
#ifdef IOTCLOUD_HUB
  if(!strcmp(key,"config_hub"))return "invalid";
#endif
  if(prefs.isKey(key)&&type!=PT_STR)return "invalid";
  return prefs.getString(key,"{}");
}
static bool due(uint32_t now,uint32_t at){return int32_t(now-at)>=0;}
static bool saveConfigTransaction(DynamicJsonDocument &candidate,bool forceRevoke){
  if(!localAuthReady){bool saved=persistAuthConfig(candidate,ph_auth_store_empty_value());if(saved)invalidateBusinessStatusCache();return saved;}
  struct Transaction {DynamicJsonDocument *candidate;} transaction{&candidate};
  char password[33];strlcpy(password,candidate["auth"]["password"]|"",sizeof(password));
  bool saved=ph_auth_credential_commit(&localAuth,password,forceRevoke,
    [](void *opaque,const ph_auth_store *store){return persistAuthConfig(*static_cast<Transaction*>(opaque)->candidate,store);},&transaction);
  memset(password,0,sizeof(password));if(saved)invalidateBusinessStatusCache();return saved;
}
static bool saveConfig(DynamicJsonDocument &candidate){return saveConfigTransaction(candidate,false);}
static bool resetConfiguration(){
  DynamicJsonDocument empty(128);empty.to<JsonObject>();
  bool recovery=configRecovery;configRecovery=false;bool saved=saveConfigTransaction(empty,true);if(!saved)configRecovery=recovery||configRecovery;return saved;
}
static void startAccessPoint(){
  const IPAddress address(192,168,0,1),netmask(255,255,255,0);
  WiFi.softAPConfig(address,address,netmask);
  WiFi.softAP(apName.c_str());
}
static void startWifi(){
  WiFi.mode(WIFI_AP_STA);
  // Recovery AP remains available when the saved station is unreachable.
  startAccessPoint();
  const char *ssid=config["wifi"]["ssid"]|"";
  if(*ssid)WiFi.begin(ssid,config["wifi"]["password"]|"");
  connectAt=millis();
}
#include "sensors.inc"
#include "indicators.inc"
#include "lan_pairing.inc"
#include "sessions.inc"
#include "commands.inc"
#include "http.inc"
#include "lan_business.inc"
#include "mqtt.inc"
#ifdef IOTCLOUD_HUB
#include "hub_lan.inc"
#include "hub.inc"
#include "home.inc"
#endif
// Called by loop and by bounded HAP waits; never re-enters homeSpan.poll().
static void pollLocalControls(uint32_t now){
  // GPIO34 reset key: hold >=5 s to reopen provisioning, retaining device settings.
  static uint32_t pressedAt=0;static bool pressed=false,handled=false;
  if(digitalRead(34)==LOW){
    if(!pressed){pressed=true;handled=false;pressedAt=now;}
    else if(!handled&&!otaActive&&uint32_t(now-pressedAt)>=5000){handled=true;startWifi();}
  }else{pressed=false;handled=false;}
  pollIndicators(now);
  if(!otaActive)pollSensors(now);
  pollBusinessStatusCache(now);
}
void setup(){
  Serial.begin(115200);
  if(!prefs.begin("daikin",false)){Serial.println("NVS unavailable");while(true)delay(1000);}
  {
    String text=loadAuthConfigText();
    if(deserializeJson(config,text)||!config.is<JsonObject>()||!validConfig(config.as<JsonObjectConst>())){
      configRecovery=true;config.clear();config.to<JsonObject>();Serial.println("Configuration recovery required");
    }
  }
  config.shrinkToFit();
  updatePassword();initLocalAuth();indicator=config["indicator"]|false;
  customLightSettings=daikin::readLightSettings(config["indicator_rules"],lightSettings);
  WiFi.mode(WIFI_AP_STA);deviceId=WiFi.macAddress();deviceId.replace(":","");deviceId.toLowerCase();
  apName="BRY88AX151K-AP-"+deviceId.substring(6);apName.toUpperCase();
  startWifi();
  pmBus.begin(23,22,100000);thBus.begin(19,21,100000);pmBus.setTimeOut(50);thBus.setTimeOut(50);
  co2Uart.begin(9600,SERIAL_8N1,16,17);hchoUart.begin(9600,SERIAL_8N1,5,18);
  for(int p:{4,25,32,33}){pinMode(p,OUTPUT);digitalWrite(p,LOW);}
  pinMode(34,INPUT); // External pull resistor is required on input-only GPIO34.
  const uint8_t hdcConfig[]={2,0x10,0};writeI2c(thBus,0x40,hdcConfig,3);
  const uint8_t hchoMode[]={0xff,1,0x78,0x41,0,0,0,0,0x46};hchoUart.write(hchoMode,9);
  httpd_config_t server=HTTPD_DEFAULT_CONFIG();server.stack_size=10240;server.recv_wait_timeout=10;server.max_uri_handlers=48;
#ifdef IOTCLOUD_HUB
  server.uri_match_fn=httpd_uri_match_wildcard;
#endif
  if(httpd_start(&web,&server)==ESP_OK){
    httpd_uri_t root={};root.uri="/";root.method=HTTP_GET;root.handler=rootHandler;httpd_register_uri_handler(web,&root);
    httpd_uri_t info={};info.uri="/api/v1/info";info.method=HTTP_GET;info.handler=infoHandler;httpd_register_uri_handler(web,&info);
    for(auto path:{"/api/v1/auth/login","/api/v1/auth/logout"}){httpd_uri_t auth={};auth.uri=path;auth.method=HTTP_POST;auth.handler=strstr(path,"logout")?logoutHandler:loginHandler;httpd_register_uri_handler(web,&auth);}
    httpd_uri_t status={};status.uri="/api/v1/status";status.method=HTTP_GET;status.handler=statusHandler;httpd_register_uri_handler(web,&status);
    for(auto path:{"/api/v1/auth/login","/api/v1/auth/logout","/api/v1/info","/api/v1/status","/api/v1/command","/api/v1/ota/upload","/api/v1/ota/upload-unsigned","/api/v1/config/backup","/api/v1/config/restore"}){httpd_uri_t options={};options.uri=path;options.method=HTTP_OPTIONS;options.handler=optionsHandler;httpd_register_uri_handler(web,&options);}
    for(auto path:{"/api/v1/config/backup","/api/v1/config/restore"}){httpd_uri_t backup={};backup.uri=path;backup.method=strstr(path,"restore")?HTTP_POST:HTTP_GET;backup.handler=backupHandler;httpd_register_uri_handler(web,&backup);}
    httpd_uri_t cmd={};cmd.uri="/api/v1/command";cmd.method=HTTP_POST;cmd.handler=commandHandler;httpd_register_uri_handler(web,&cmd);
    for(auto path:{"/api/v1/ota/upload","/api/v1/ota/upload-unsigned"}){httpd_uri_t ota={};ota.uri=path;ota.method=HTTP_POST;ota.handler=uploadHandler;httpd_register_uri_handler(web,&ota);}
#ifndef IOTCLOUD_HUB
    httpd_uri_t convert={};convert.uri="/api/v1/ota/convert";convert.method=HTTP_POST;convert.handler=convertHandler;httpd_register_uri_handler(web,&convert);
    convert.method=HTTP_OPTIONS;convert.handler=optionsHandler;httpd_register_uri_handler(web,&convert);
#endif
  }
#ifdef IOTCLOUD_HUB
  sensorBoot=esp_random()|1;
  registerHubHttp();hubInit();initHubLan();homeInit();
#endif
  // Confirm local startup once the command server is available.
  if(web)esp_ota_mark_app_valid_cancel_rollback();
  if(!sensorBoot)sensorBoot=esp_random()|1;sensorUdp.begin(11288);
  startMqtt();Serial.printf("DAIKIN BRY88AX151K %s %s\n",pmModel,deviceId.c_str());
}
void loop(){
  uint32_t now=millis();
  pollLocalControls(now);
  if(!otaActive){pollSessions();pollLanPairing();}
  pollBusinessStatusCache(now);pollLanBusiness();
  if(webPending){
    if(webAuthRequired&&!localAuthContextValid(&webAuthContext)){
      webResult="{\"code\":\"unauthorized\"}";
#ifdef IOTCLOUD_HUB
      hubHttpStatus=401;
#endif
    }else{
#ifdef IOTCLOUD_HUB
    if(webOperation==3){if(otaActive){hubHttpStatus=503;hubHttpToken="";webResult="{\"code\":\"busy\"}";}else hubApiExecute();}else
#endif
    if(webOperation==4){if(businessStatusReady)webResult=cachedHttpStatus();else webResult="{\"code\":\"busy\"}";}else webResult=otaActive?"{\"code\":\"busy\",\"message\":\"OTA active\"}":webOperation?backupExecute():execute(webBody);}
    portENTER_CRITICAL(&webMux);webPending=false;webDone=true;portEXIT_CRITICAL(&webMux);}
  if(mqttQueued){String body(mqttPending,mqttReceived);bool sensor=mqttSensorCommand;portENTER_CRITICAL(&mqttMux);mqttQueued=false;portEXIT_CRITICAL(&mqttMux);if(!otaActive){if(sensor)sessionGrant(body);else if(!sessionGrant(body))publish(mqttRoot+"/result",execute(body),false);}}
  if(mqttOnline){mqttOnline=false;
    DynamicJsonDocument doc(1024);capabilities(doc.to<JsonObject>());String text;serializeWireMP(doc,text);publish(mqttRoot+"/capabilities",text);publish(mqttRoot+"/availability","online");
    String discoveryTopic=mqttRoot.substring(0,mqttRoot.indexOf("/daikin/"))+"/discovery/"+deviceId;
    doc.clear();doc["schema"]=2;doc["model"]="daikin";doc["protocol"]="tmcp";doc["protocol_version"]=2;doc["version"]="v2";
    doc["device_id"]=deviceId;doc["firmware_version"]=version;doc["variant"]=DAIKIN_VARIANT;doc["wire_version"]=1;
    doc.createNestedArray("wire_formats").add("msgpack");doc["default_wire_format"]="msgpack";
    text="";serializeWireMP(doc,text);publish(discoveryTopic,text);haBirth=true;
    doc.clear();doc["ip"]=WiFi.localIP().toString();text="";serializeWireMP(doc,text);
    publish(sensorRoot.substring(0,sensorRoot.indexOf("/sensor/"))+"/address/"+deviceId,text);
  }
  static bool previousHcho=false,previousPm10=false;
  if(previousHcho!=hchoPresent||previousPm10!=sensorPresent(PM10)){previousHcho=hchoPresent;previousPm10=sensorPresent(PM10);haBirth=true;}
  #ifdef IOTCLOUD_HUB
  pollHub(now);
  const bool outputReady=mqttConnected||hubReady();
#else
  const bool outputReady=mqttConnected;
#endif
  if(haBirth&&outputReady){haBirth=false;discovery();}
  if(outputReady&&due(now,nextPublish)){nextPublish=now+5000;publishState();}
  if(mqttRestart){mqttRestart=false;startMqtt();}
  if(wifiRestart){wifiRestart=false;startWifi();}
  pollIndicators(now);
  // Close initial recovery AP after station has been stable for two minutes.
  static uint32_t connectedSince=0;
  if(WiFi.status()==WL_CONNECTED){if(!connectedSince){connectedSince=now;if(MDNS.begin(("daikin-"+deviceId).c_str())){MDNS.addService("http","tcp",80);advertiseLanBusiness();}}if(!configRecovery&&uint32_t(now-connectedSince)>120000&&uint32_t(now-connectAt)>120000)WiFi.softAPdisconnect(true);}
  else{if(connectedSince){
#ifdef IOTCLOUD_HUB
    hubLanMdnsEnding();
#endif
    MDNS.end();}connectedSince=0;if(!(WiFi.getMode()&WIFI_AP))startAccessPoint();}
  #ifdef IOTCLOUD_HUB
  pollHome(millis());
  #endif
  if(!otaActive&&rebootAt&&due(now,rebootAt))ESP.restart();
  // Local serial console uses the same bounded command dispatcher. Credentials
  // are accepted for provisioning but are never echoed or returned in queries.
  static String serialBody;static bool serialOverflow=false;
  while(Serial.available()){
    char c=Serial.read();if(c=='\r')continue;
    if(c=='\n'){if(!serialOverflow&&serialBody.length())Serial.println(otaActive?"{\"code\":\"busy\"}":execute(serialBody,false));serialBody="";serialOverflow=false;}
    else if(!serialOverflow){if(serialBody.length()<4096)serialBody+=c;else{serialBody="";serialOverflow=true;}}
  }
  delay(2);
}
