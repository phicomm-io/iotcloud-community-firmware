#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <mqtt_client.h>
#include <mbedtls/base64.h>
#include "frames.h"
extern "C" {
#include "../../common/ota/signed_ota.h"
}
#include "web.h"

// BRY88AX151K family, tested BRY88AB151K board wiring; see README.
static TwoWire pmBus(0), thBus(1);
static HardwareSerial co2Uart(2), hchoUart(1);
enum Sensor { PM25, CO2, TEMPERATURE, HUMIDITY, TVOC, HCHO, PM10, SENSOR_COUNT };
static daikin::Sample readings[SENSOR_COUNT];
static const char *keys[]={"pm25","co2","temperature","humidity","tvoc","formaldehyde","pm10"};
static const char *units[]={"µg/m³","ppm","°C","%","ppb","mg/m³","µg/m³"};
static Preferences prefs;
static httpd_handle_t web;
static esp_mqtt_client_handle_t mqtt=nullptr;
static volatile bool mqttConnected=false;
static volatile int mqttLastEvent=MQTT_EVENT_ANY,mqttErrorType=0,mqttSocketError=0;
static bool hchoPresent=false, indicator=false;
static bool sensorPresent(unsigned i){return i==HCHO?hchoPresent:i!=PM10||readings[PM10].received;}
static int commandSubId=-1;
static bool sgpReady=false;
static uint32_t sgpSince=0, nextSample=0, nextSgp=0, nextPublish=0, connectAt=0;
static volatile uint32_t rebootAt=0;
static String deviceId, apName, mqttRoot, configText;
static portMUX_TYPE mqttMux=portMUX_INITIALIZER_UNLOCKED;
static char mqttPending[4097];
static volatile bool mqttQueued=false;
static bool mqttReceiving=false;
static size_t mqttReceived=0;
static DynamicJsonDocument config(3072);
static String lastRequest[8], lastBody[8], lastResponse[8];
static unsigned requestSlot=0;
static uint8_t co2Frame[8], hchoFrame[9];
static size_t co2Used=0, hchoUsed=0;
static bool co2Waiting=false, hchoWaiting=false;
static uint32_t co2Sent=0, hchoSent=0;
static bool mqttRestart=false, wifiRestart=false;
#include "build_info.h"
static const char *version=DAIKIN_VERSION;
extern "C" {
extern const char daikin_target[32] __attribute__((section(".rodata_custom_desc"),used)) = DAIKIN_TARGET;
}
static char webPassword[33]={};
static portMUX_TYPE authMux=portMUX_INITIALIZER_UNLOCKED;
static void updatePassword(){
  const char *pass=config["auth"]["password"]|"";
  portENTER_CRITICAL(&authMux);strlcpy(webPassword,pass,sizeof(webPassword));portEXIT_CRITICAL(&authMux);
}
static const char *pmModel="detecting";

static bool due(uint32_t now,uint32_t at){return int32_t(now-at)>=0;}
static bool saveConfig(DynamicJsonDocument &candidate){
  String text; serializeJson(candidate,text);
  if(prefs.putString("config",text)!=text.length())return false;
  config.clear(); config.set(candidate);configText=text; updatePassword();return true;
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
#include "sessions.inc"
#include "commands.inc"
#include "http.inc"
#include "mqtt.inc"
void setup(){
  Serial.begin(115200);
  if(!prefs.begin("daikin",false)){Serial.println("NVS unavailable");while(true)delay(1000);}
  configText=prefs.getString("config","{}");if(deserializeJson(config,configText)||!config.is<JsonObject>()){Serial.println("Config corrupt; refusing to discard credentials");while(true)delay(1000);}
  updatePassword();indicator=config["indicator"]|false;
  WiFi.mode(WIFI_AP_STA);deviceId=WiFi.macAddress();deviceId.replace(":","");deviceId.toLowerCase();
  apName="BRY88AX151K-AP-"+deviceId.substring(6);apName.toUpperCase();
  startWifi();
  pmBus.begin(23,22,100000);thBus.begin(19,21,100000);pmBus.setTimeOut(50);thBus.setTimeOut(50);
  co2Uart.begin(9600,SERIAL_8N1,16,17);hchoUart.begin(9600,SERIAL_8N1,5,18);
  for(int p:{4,25,32,33}){pinMode(p,OUTPUT);digitalWrite(p,LOW);}
  pinMode(34,INPUT); // External pull resistor is required on input-only GPIO34.
  const uint8_t hdcConfig[]={2,0x10,0};writeI2c(thBus,0x40,hdcConfig,3);
  const uint8_t hchoMode[]={0xff,1,0x78,0x41,0,0,0,0,0x46};hchoUart.write(hchoMode,9);
  httpd_config_t server=HTTPD_DEFAULT_CONFIG();server.stack_size=10240;server.recv_wait_timeout=10;server.max_uri_handlers=12;
  if(httpd_start(&web,&server)==ESP_OK){
    httpd_uri_t root={};root.uri="/";root.method=HTTP_GET;root.handler=rootHandler;httpd_register_uri_handler(web,&root);
    httpd_uri_t info={};info.uri="/api/v1/info";info.method=HTTP_GET;info.handler=infoHandler;httpd_register_uri_handler(web,&info);
    for(auto path:{"/api/v1/info","/api/v1/command","/api/v1/ota/upload","/api/v1/ota/upload-unsigned"}){httpd_uri_t options={};options.uri=path;options.method=HTTP_OPTIONS;options.handler=optionsHandler;httpd_register_uri_handler(web,&options);}
    httpd_uri_t cmd={};cmd.uri="/api/v1/command";cmd.method=HTTP_POST;cmd.handler=commandHandler;httpd_register_uri_handler(web,&cmd);
    for(auto path:{"/api/v1/ota/upload","/api/v1/ota/upload-unsigned"}){httpd_uri_t ota={};ota.uri=path;ota.method=HTTP_POST;ota.handler=uploadHandler;httpd_register_uri_handler(web,&ota);}
  }
  // Confirm local startup once the command server is available.
  if(web)esp_ota_mark_app_valid_cancel_rollback();
  sensorBoot=esp_random()|1;sensorUdp.begin(11288);
  startMqtt();Serial.printf("DAIKIN BRY88AX151K %s %s\n",pmModel,deviceId.c_str());
}
void loop(){
  uint32_t now=millis();
  if(!otaActive){pollSensors(now);pollSessions();}
  if(webPending){webResult=otaActive?"{\"code\":\"busy\",\"message\":\"OTA active\"}":execute(webBody);portENTER_CRITICAL(&webMux);webPending=false;webDone=true;portEXIT_CRITICAL(&webMux);}
  if(mqttQueued){String body(mqttPending);bool sensor=mqttSensorCommand;portENTER_CRITICAL(&mqttMux);mqttQueued=false;portEXIT_CRITICAL(&mqttMux);if(!otaActive){if(sensor)sessionGrant(body);else if(!sessionGrant(body))publish(mqttRoot+"/result",execute(body),false);}}
  if(mqttOnline){mqttOnline=false;
    DynamicJsonDocument doc(1024);capabilities(doc.to<JsonObject>());String text;serializeJson(doc,text);publish(mqttRoot+"/capabilities",text);publish(mqttRoot+"/availability","online");
    String discoveryTopic=mqttRoot.substring(0,mqttRoot.indexOf("/daikin/"))+"/discovery/"+deviceId;
    publish(discoveryTopic,"{\"schema\":1,\"model\":\"daikin\",\"protocol\":\"tmcp\",\"protocol_version\":2,\"version\":\"v2\"}");haBirth=true;
    publish(sensorRoot.substring(0,sensorRoot.indexOf("/sensor/"))+"/address/"+deviceId,String("{\"ip\":\"")+WiFi.localIP().toString()+"\"}");
  }
  static bool previousHcho=false,previousPm10=false;
  if(previousHcho!=hchoPresent||previousPm10!=sensorPresent(PM10)){previousHcho=hchoPresent;previousPm10=sensorPresent(PM10);haBirth=true;}
  if(haBirth&&mqttConnected){haBirth=false;discovery();}
  if(mqttConnected&&due(now,nextPublish)){nextPublish=now+5000;publishState();}
  if(mqttRestart){mqttRestart=false;startMqtt();}
  if(wifiRestart){wifiRestart=false;startWifi();}
  // GPIO34 reset key: hold >=5 s to reopen provisioning, retaining device settings.
  static uint32_t pressedAt=0;static bool pressed=false,handled=false;
  if(digitalRead(34)==LOW){
    if(!pressed){pressed=true;handled=false;pressedAt=now;}
    else if(!handled&&!otaActive&&uint32_t(now-pressedAt)>=5000){handled=true;startWifi();}
  }else{pressed=false;handled=false;}
  // Close initial recovery AP after station has been stable for two minutes.
  static uint32_t connectedSince=0;
  if(WiFi.status()==WL_CONNECTED){if(!connectedSince){connectedSince=now;if(MDNS.begin(("daikin-"+deviceId).c_str()))MDNS.addService("http","tcp",80);}if(uint32_t(now-connectedSince)>120000&&uint32_t(now-connectAt)>120000)WiFi.softAPdisconnect(true);}
  else{if(connectedSince)MDNS.end();connectedSince=0;if(!(WiFi.getMode()&WIFI_AP))startAccessPoint();}
  if(!otaActive&&rebootAt&&due(now,rebootAt))ESP.restart();
  // Local serial console uses the same bounded command dispatcher. Credentials
  // are accepted for provisioning but are never echoed or returned in queries.
  static String serialBody;static bool serialOverflow=false;
  while(Serial.available()){
    char c=Serial.read();if(c=='\r')continue;
    if(c=='\n'){if(!serialOverflow&&serialBody.length())Serial.println(otaActive?"{\"code\":\"busy\"}":execute(serialBody));serialBody="";serialOverflow=false;}
    else if(!serialOverflow){if(serialBody.length()<4096)serialBody+=c;else{serialBody="";serialOverflow=true;}}
  }
  delay(2);
}
