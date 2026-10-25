// Compile the actual firmware restore/reset, save and quiesce functions. Only
// NVS, ESP MQTT tasks, HAP socket closing and clocks are replaced on the host.
#include <ArduinoJson.h>
#include "hub_home.h"
#include "hub_relay.h"
#include "hub_transport.h"
#include "indicator_json.h"
#include "climate.h"
#include "i18n.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define IOTCLOUD_HUB 1
#define DAIKIN_VARIANT "hub"
struct String:std::string {
  using std::string::string;
  String(const std::string &s):std::string(s){}
  bool isEmpty()const{return empty();}
  bool endsWith(const char *s)const{size_t n=strlen(s);return size()>=n&&compare(size()-n,n,s)==0;}
  int indexOf(const String &s)const{auto n=find(s);return n==npos?-1:int(n);}
  size_t write(uint8_t c){push_back(c);return 1;}
  size_t write(const uint8_t *p,size_t n){append(reinterpret_cast<const char *>(p),n);return n;}
};
namespace ArduinoJson {
template<> struct Converter<String> {
  static String fromJson(JsonVariantConst value){return value.as<std::string>();}
  static void toJson(const String &value,JsonVariant out){out.set(value.c_str());}
};
}
static constexpr size_t configCapacity=16384;
static DynamicJsonDocument config(configCapacity);
static bool configRecovery=false;
static String configText,webBody,webResult,hubHttpToken;
static unsigned webOperation=2;
static int hubHttpStatus=200;
static uint32_t now=100,rebootAt=0;
static uint32_t millis(){return now;}
static const char *version="1.0.0";
static unsigned passwordUpdates=0;
static void updatePassword(){++passwordUpdates;}
static bool hubRestartPending();
static void hubRestoreChanged();
struct Prefs {
  bool fail=false,clearFail=false;
  unsigned writes=0,clears=0;
  std::string persisted;
  size_t putBytes(const char *,const char *p,size_t n){++writes;if(fail)return 0;persisted.assign(p,n);return n;}
  bool clear(){++clears;if(clearFail)return false;persisted.clear();return true;}
};
static Prefs prefs;
#include "save.inc"
static iotcloud::HubService hubService(config);
static iotcloud::HubTransport hubTransport;
static iotcloud::HubRelay hubRelay(config,hubService,hubTransport.broker);
static iotcloud::HubHome hubHome(hubTransport.broker);
using Request=iotcloud::HubService::Request;
static Request hubHttpRequest("GET","/api/auth/me");
typedef void *esp_mqtt_client_handle_t;
typedef void *QueueHandle_t;
static esp_mqtt_client_handle_t mqtt=reinterpret_cast<void *>(1);
static bool mqttConnected=true,mqttCommandReady=true,mqttOnline=true,haBirth=false;
static bool mqttReceiving=true,mqttQueued=true,mqttRestart=true,wifiRestart=true;
static bool mqttSensorCommand=true;
static size_t mqttReceived=5;
static char mqttPending[4097]="old-command";
static int mqttMux=0;
#define portENTER_CRITICAL(mux) (void)(mux)
#define portEXIT_CRITICAL(mux) (void)(mux)
struct SensorSession {bool used=false,active=false;};
static SensorSession sessions[4];
static unsigned stopped=0,destroyed=0,queueResets=0;
static void esp_mqtt_client_stop(esp_mqtt_client_handle_t){++stopped;}
static void esp_mqtt_client_destroy(esp_mqtt_client_handle_t){++destroyed;}
static void xQueueReset(QueueHandle_t){++queueResets;}
#include "hub-globals.inc"
#include "mqtt-stop.inc"
#include "bemfa-stop.inc"
static bool homeStarted=true,homePaused=false;
static const char *homeError="ready";
static unsigned hapStops=0;
static struct {void iotcloudStop(){++hapStops;}} homeSpan;
#include "home-quiesce.inc"
static bool homeFactoryReset(){return true;}
static struct {unsigned getFreeHeap(){return 200000;}} ESP;
#include "quiesce.inc"
static unsigned homeChanges=0,bemfaStarts=0;
static void homeChanged(){++homeChanges;}
static void startBemfa(){++bemfaStarts;}
static constexpr int pdTRUE=1;
static int xQueueReceive(QueueHandle_t,void *,int){return 0;}
static uint32_t nextPublish=0;
static bool otaActive=false;
#include "poll.inc"
static void state(JsonObject){}
static void sessionStatus(JsonObject){}
static void capabilities(JsonObject){}
static constexpr int WIFI_SCAN_RUNNING=-1,WIFI_AUTH_OPEN=0;
static struct {
  int scanComplete(){return 0;}void scanDelete(){}void scanNetworks(bool){}
  const char *SSID(int){return "";}int RSSI(int){return 0;}int encryptionType(int){return 0;}
} WiFi;
static bool indicator=false,customLightSettings=false;
static daikin::LightSettings lightSettings=daikin::defaultLightSettings();
static struct {int target=400;bool request(int,uint32_t){return false;}} co2Module;
enum Sensor {CO2,TEMPERATURE,HUMIDITY,SENSOR_COUNT};
static daikin::Sample readings[SENSOR_COUNT];
static uint32_t nextSample=0;
static String lastRequest[8],lastBody[8],lastResponse[8];
static unsigned requestSlot=0;
#include "commands.inc"
#include "backup.inc"

static std::string hex(const uint8_t *p,size_t n){
  std::string result;
  for(size_t i=0;i<n;++i){result+="0123456789abcdef"[p[i]>>4];result+="0123456789abcdef"[p[i]&15];}
  return result;
}
static void text(std::vector<uint8_t> &out,const std::string &s){
  out.push_back(s.size()>>8);out.push_back(s.size()&255);out.insert(out.end(),s.begin(),s.end());
}
static int connectClient(uint16_t port){
  int fd=::socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
  sockaddr_in address={};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
  assert(::connect(fd,reinterpret_cast<sockaddr *>(&address),sizeof address)==0);
  std::vector<uint8_t> body={0,4,'M','Q','T','T',4,0xc2,0,30};
  text(body,"old-client");text(body,"oldowner");text(body,"old-password");
  std::vector<uint8_t> packet={0x10,uint8_t(body.size())};packet.insert(packet.end(),body.begin(),body.end());
  assert(::send(fd,packet.data(),packet.size(),0)==int(packet.size()));
  for(unsigned i=0;i<100&&hubTransport.broker.count()!=1;++i)hubTransport.poll(now++);
  assert(hubTransport.broker.count()==1);
  uint8_t ack[4];assert(::recv(fd,ack,sizeof ack,0)==4&&ack[0]==0x20&&ack[3]==0);
  return fd;
}
static void assertLive(const std::string &token,int fd){
  assert(!hubRestartPending()&&hubReady()&&hubTransport.broker.count()==1);
  assert(hubService.user()=="oldowner");
  assert(hubService.handle(Request("GET","/api/auth/me","",token),now).status==200);
  assert(!homePaused&&hapStops==0&&stopped==0&&destroyed==0&&queueResets==0);
  assert(sessions[0].used&&mqttQueued&&mqttReceiving&&mqtt&&bemfaMqtt);
  uint8_t byte;assert(::recv(fd,&byte,1,MSG_DONTWAIT)<0);
}
static void assertQuiesced(const std::string &token,int fd){
  assert(hubRestartPending()&&!hubReady()&&!hubTransport.running());
  assert(hubTransport.broker.count()==0&&hubTransport.broker.retainedCount()==0);
  assert(hubService.handle(Request("GET","/api/auth/me","",token),now).status==401);
  assert(homePaused&&hapStops==1&&!strcmp(homeError,"restart_required"));
  for(const auto &d:hubHome.devices)assert(!d.online&&!d.controls);
  assert(stopped==2&&destroyed==2&&!mqtt&&!bemfaMqtt&&!mqttConnected&&!bemfaConnected);
  assert(!mqttQueued&&!mqttReceiving&&!mqttReceived&&!mqttPending[0]);
  for(const auto &s:sessions)assert(!s.used&&!s.active);
  assert(hubEpoch==1&&queueResets==1&&hubHttpToken.empty());
  assert(hubRemoteTopics.empty()&&bemfaTopics.empty()&&bemfaConfiguredKey.empty());
  for(unsigned i=0;i<3;++i)assert(!hubMessages[i].receiving&&!hubMessages[i].length);
  uint8_t byte;assert(::recv(fd,&byte,1,0)==0);
  hubReload=true;hubCredentials=true;pollHub(++now);
  assert(!hubTransport.running()&&homeChanges==0&&bemfaStarts==0);
  hubHttpToken=token;hubApiExecute();assert(hubHttpStatus==503&&hubHttpToken.empty());
  unsigned writes=prefs.writes;
  DynamicJsonDocument next(configCapacity);next.set(config);assert(!saveConfig(next)&&prefs.writes==writes);
  assert(execute(R"({"request_id":"after","action":"config.set","params":{"section":"device","values":{"name":"rewrite"}}})").find("restart_required")!=String::npos);
  assert(prefs.writes==writes&&backupExecute().find("restart_required")!=String::npos);
}
int main(int argc,char **argv){
  assert(argc==4);
  std::string scenario=argv[1];
  hubService.randomHex=[](size_t n){uint8_t bytes[32];assert(n<=sizeof bytes&&RAND_bytes(bytes,n)==1);return hex(bytes,n);};
  hubService.derive=[](const std::string &p,const std::string &s){uint8_t bytes[32];assert(PKCS5_PBKDF2_HMAC(p.data(),p.size(),reinterpret_cast<const uint8_t *>(s.data()),s.size(),10000,EVP_sha256(),sizeof bytes,bytes)==1);return hex(bytes,sizeof bytes);};
  hubService.store=saveConfig;
  config["auth"]["password"]="device-password";
  assert(hubService.handle(Request("POST","/api/hub/setup",R"({"username":"oldowner","password":"old-password"})","","",true),now).status==201);
  auto login=hubService.handle(Request("POST","/api/auth/login",R"({"username":"oldowner","password":"old-password"})"),++now);
  assert(login.status==200&&!login.token.empty());
  hubTransport.broker.authenticate=[](const std::string &u,const std::string &p){return hubService.authenticate(u,p,now);};
  hubTransport.broker.authorize=[](const std::string &t,bool w){return hubService.allowed(t,w);};
  assert(hubTransport.start(atoi(argv[2]),atoi(argv[3])));
  int fd=connectClient(atoi(argv[2]));
  bemfaMqtt=reinterpret_cast<void *>(2);bemfaConnected=true;bemfaConfiguredKey="old-key";
  hubRemoteTopics={"old/upstream"};bemfaTopics={"old/bemfa"};hubHttpToken=login.token.c_str();
  hubInbox=reinterpret_cast<void *>(3);hubMessages.reset(new HubMessage[3]);
  for(unsigned i=0;i<3;++i){hubMessages[i].receiving=true;hubMessages[i].length=5;}
  sessions[0].used=sessions[0].active=true;hubHome.devices[0].online=hubHome.devices[0].controls=true;
  hubReload=hubCredentials=hubBirth=true;
  if(scenario=="mqtt"||scenario=="mqtt-local"||scenario=="mqtt-fail"){
    hubReload=hubCredentials=hubBirth=mqttRestart=false;
    prefs.fail=scenario=="mqtt-fail";
    auto before=prefs.persisted;
    String request=R"({"request_id":"mqtt-change","action":"config.set","params":{"section":"mqtt","values":{"host":"new-broker.example","password":"new-secret"}}})",result;
    if(scenario=="mqtt-local"){
      const std::string topic=hubService.prefix()+"daikin/aabbccddeeff/command";
      hubTransport.broker.event=[&](const std::string &t,const std::string &body,bool,iotcloud::HubBroker::Origin origin){
        if(t==topic&&origin==iotcloud::HubBroker::Origin::Local)result=execute(body);
      };
      std::vector<uint8_t> body;text(body,topic);body.insert(body.end(),request.begin(),request.end());
      std::vector<uint8_t> packet={0x30};size_t length=body.size();
      do{uint8_t byte=length%128;length/=128;packet.push_back(byte|(length?128:0));}while(length);
      packet.insert(packet.end(),body.begin(),body.end());
      assert(::send(fd,packet.data(),packet.size(),0)==int(packet.size()));
      for(unsigned i=0;i<100&&result.empty();++i)pollHub(++now);
      assert(!result.empty());
    }else result=execute(request);
    if(prefs.fail){
      assert(result.find("storage_error")!=String::npos&&prefs.persisted==before);
      assertLive(login.token,fd);assert(!hubReload&&!mqttRestart&&hubEpoch==0);
    }else{
      assert(result.find("accepted")!=String::npos&&config["mqtt"]["host"]=="new-broker.example");
      assert(!mqtt&&!mqttConnected&&!mqttQueued&&!mqttReceiving&&!mqttReceived&&!mqttPending[0]);
      assert(bemfaMqtt&&bemfaConnected&&stopped==1&&destroyed==1);
      assert(hubReload&&!mqttRestart&&hubEpoch==0&&queueResets==0);
      assert(hubReady()&&hubTransport.broker.count()==1&&!homePaused&&hapStops==0);
      assert(hubService.handle(Request("GET","/api/auth/me","",login.token),now).status==200);
      pollHub(++now);
      assert(hubEpoch==1&&queueResets==1&&stopped==2&&destroyed==2);
      assert(!hubReload&&mqttRestart&&!mqtt&&!bemfaMqtt&&bemfaStarts==1);
      for(unsigned i=0;i<3;++i)assert(!hubMessages[i].receiving&&!hubMessages[i].length);
    }
  }else if(scenario=="reset"||scenario=="reset-fail"){
    prefs.clearFail=scenario=="reset-fail";
    auto result=execute(R"({"request_id":"reset","action":"system.reset","params":{"full":true}})");
    if(prefs.clearFail){assert(result.find("storage_error")!=String::npos);assert(!prefs.persisted.empty());assertLive(login.token,fd);assert(!rebootAt);}
    else{assert(result.find("accepted")!=String::npos);assert(prefs.persisted.empty());assertQuiesced(login.token,fd);assert(rebootAt);}
  }else{
    DynamicJsonDocument backup(configCapacity+1024);
    backup["schema"]=1;backup["model"]="daikin";backup["variant"]="hub";backup["config_version"]=1;backup["config"]=config;
    if(scenario!="same")backup["config"]["hub"]["username"]="newowner";
    assert(validConfig(backup["config"]));serializeJson(backup,webBody);
    if(scenario=="restore-fail"){
      auto before=prefs.persisted;prefs.fail=true;
      assert(backupExecute().find("storage_error")!=String::npos);assert(prefs.persisted==before);assertLive(login.token,fd);assert(!rebootAt);
      prefs.fail=false;backup["variant"]="full";webBody="";serializeJson(backup,webBody);
      assert(backupExecute().find("invalid_backup")!=String::npos);assertLive(login.token,fd);assert(!rebootAt);
    }else{
      assert(backupExecute().find("accepted")!=String::npos);
      assert(hubService.user()==(scenario=="same"?"oldowner":"newowner"));assertQuiesced(login.token,fd);assert(rebootAt);
    }
  }
  ::close(fd);
  puts(("production hub "+scenario+" lifecycle passed").c_str());
}
