// Compile the actual firmware restore/reset, save and quiesce functions. Only
// NVS, ESP MQTT tasks, HAP socket closing and clocks are replaced on the host.
#include <algorithm>
#include <cstdlib>
#include <stdlib.h>
#include <cstdint>
#include <string>
// Limit the pinned ArduinoJson allocator, leaving protocol/socket bookkeeping
// independent. This reproduces a largest available block, not a fake JSON doc.
static size_t jsonAllocationLimit=SIZE_MAX,jsonAllocationCount=0,jsonFailAt=0;
static size_t deniedJsonAllocations=0;
static void *jsonAllocate(size_t size){
  if(++jsonAllocationCount==jsonFailAt||size>jsonAllocationLimit){++deniedJsonAllocations;return nullptr;}
  return std::malloc(size);
}
static void *jsonReallocate(void *pointer,size_t size){
  if(size>jsonAllocationLimit){++deniedJsonAllocations;return nullptr;}
  return std::realloc(pointer,size);
}
#define malloc jsonAllocate
#define realloc jsonReallocate
#include <ArduinoJson.h>
#undef malloc
#undef realloc
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
static size_t stringAllocationLimit=SIZE_MAX,stringWriteLimit=SIZE_MAX,stringConcatLimit=SIZE_MAX;
struct String:std::string {
  using std::string::string;
  using std::string::append;
  String(const std::string &s):std::string(s){}
  bool isEmpty()const{return empty();}
  bool startsWith(const char *s)const{return compare(0,strlen(s),s)==0;}
  bool endsWith(const char *s)const{size_t n=strlen(s);return size()>=n&&compare(size()-n,n,s)==0;}
  int indexOf(const String &s)const{auto n=find(s);return n==npos?-1:int(n);}
  bool reserve(size_t n){if(n>stringAllocationLimit)return false;std::string::reserve(n);return true;}
  void push_back(char c){if(size()<stringWriteLimit)std::string::push_back(c);}
  String &append(const char *p,size_t n){n=std::min(n,stringWriteLimit>size()?stringWriteLimit-size():size_t(0));std::string::append(p,n);return *this;}
  bool concat(const char *p,size_t n){if(size()+n>stringConcatLimit||size()+n>stringAllocationLimit)return false;append(p,n);return true;}
  size_t write(uint8_t c){return write(&c,1);}
  size_t write(const uint8_t *p,size_t n){n=std::min(n,stringWriteLimit>size()?stringWriteLimit-size():size_t(0));append(reinterpret_cast<const char *>(p),n);return n;}
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
static size_t hubByteHeap(){return 200000;}
static size_t hubByteBlock(){return 200000;}
static bool hubAllocationReady(size_t n){return hubByteHeap()>n+16384&&hubByteBlock()>n;}
#include "quiesce.inc"
static unsigned homeChanges=0,bemfaStarts=0;
static void homeChanged(){++homeChanges;}
static void startBemfa(){++bemfaStarts;}
static constexpr int pdTRUE=1;
static int xQueueReceive(QueueHandle_t,void *,int){return 0;}
static unsigned uxQueueMessagesWaiting(QueueHandle_t){return 0;}
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

// Run the production HTTP handler too: delay() dispatches its queued operation
// as loop() would. Authentication itself is covered by the OTA handler suite.
using esp_err_t=int;
static constexpr int ESP_OK=0,ESP_FAIL=-1,HTTP_GET=0,HTTP_POST=1;
using std::min;
struct httpd_req_t {
  int method=HTTP_GET,content_len=0;
  size_t offset=0;
  std::string input,status="200 OK",type,body,cache;
};
static int authMux=0,webMux=0;
static char webPassword[33]="device-password";
static bool webPending=false,webDone=true;
static void httpd_resp_set_hdr(httpd_req_t *req,const char *key,const char *value){if(!strcmp(key,"Cache-Control"))req->cache=value;}
static void httpd_resp_set_status(httpd_req_t *req,const char *status){req->status=status;}
static void httpd_resp_set_type(httpd_req_t *req,const char *type){req->type=type;}
static esp_err_t httpd_resp_send(httpd_req_t *req,const char *body,size_t length){req->body.assign(body?body:"",length);return ESP_OK;}
static int httpd_req_recv(httpd_req_t *req,char *out,int size){size_t n=std::min(size_t(size),req->input.size()-req->offset);memcpy(out,req->input.data()+req->offset,n);req->offset+=n;return int(n);}
static bool authorized(httpd_req_t *){return true;}
static void delay(unsigned n){now+=n;if(webPending&&!webDone){webResult=backupExecute();webPending=false;webDone=true;}}
#include "json-response.inc"
#include "backup-handler.inc"

static void normalMemory(){
  jsonAllocationLimit=stringAllocationLimit=stringWriteLimit=stringConcatLimit=SIZE_MAX;
  jsonFailAt=0;
}
static String restoreBody(bool larger){
  DynamicJsonDocument backup(configCapacity+1024);
  backup["schema"]=1;backup["model"]="daikin";backup["variant"]="hub";backup["config_version"]=1;backup["config"]=config;
  if(larger){
    auto h=backup["config"]["hub"].as<JsonObject>();h["username"]="newowner";
    auto devices=h["devices"].to<JsonArray>();
    for(unsigned i=0;i<12;++i){
      auto d=devices.createNestedObject();char mac[13];snprintf(mac,sizeof(mac),"aabbccdd%04x",i+1);
      d["mac"]=std::string(mac);d["model"]=i==0?"tc1":i==1?"a1":"m1";
      d["version"]="v2";d["status"]="active";d["username"]="newowner";
    }
  }
  assert(!backup.overflowed()&&validConfig(backup["config"]));
  String body;serializeJson(backup,body);return body;
}

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
  for(const auto &message:hubMessages)assert(!message);
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
  hubInbox=reinterpret_cast<void *>(3);
  for(auto &message:hubMessages){message.reset(new HubMessage);message->receiving=true;message->length=5;}
  sessions[0].used=sessions[0].active=true;hubHome.devices[0].online=hubHome.devices[0].controls=true;
  hubReload=hubCredentials=hubBirth=true;
  if(scenario.compare(0,7,"backup-")==0){
    if(scenario=="backup-fit"){
      for(unsigned i=0;i<3;++i){
        String body=std::string("{\"model\":\"m1\",\"mac\":\"aabbccdd000")+char('1'+i)+"\",\"version\":\"v2\"}";
        assert(hubService.handle(Request("POST","/api/devices",body,login.token),++now).status==201);
      }
    }
    const auto before=prefs.persisted;const auto writes=prefs.writes;
    jsonAllocationCount=deniedJsonAllocations=0;
    jsonAllocationLimit=7924;stringAllocationLimit=7924;
    if(scenario=="backup-json")jsonAllocationLimit=0;
    if(scenario=="backup-string")stringAllocationLimit=32;
    if(scenario=="backup-short")stringWriteLimit=100;
    httpd_req_t req;assert(backupHandler(&req)==ESP_OK);
    normalMemory();
    if(scenario=="backup-fit"){
      assert(req.status=="200 OK"&&req.type=="application/octet-stream"&&req.cache=="no-store");
      DynamicJsonDocument parsed(configCapacity+1024);assert(!deserializeJson(parsed,req.body));
      assert(parsed["schema"]==1&&parsed["model"]=="daikin"&&parsed["variant"]=="hub");
      assert(parsed["firmware_version"]==version&&validConfig(parsed["config"]));
      std::string saved,current;serializeJson(parsed["config"],saved);serializeJson(config,current);assert(saved==current);
      assert(deniedJsonAllocations==0);
    }else assert(req.status=="503 Service Unavailable"&&req.body=="{\"code\":\"busy\"}");
    assert(prefs.persisted==before&&prefs.writes==writes&&!rebootAt);assertLive(login.token,fd);
  }else if(scenario=="restore-fit"||scenario.compare(0,12,"restore-oom-")==0){
    httpd_req_t req;req.method=HTTP_POST;req.input=restoreBody(scenario=="restore-fit"||scenario=="restore-oom-parse");req.content_len=req.input.size();
    const auto before=prefs.persisted;const auto writes=prefs.writes;
    jsonAllocationCount=deniedJsonAllocations=0;
    jsonAllocationLimit=7924;stringAllocationLimit=7924;
    if(scenario=="restore-oom-parse")jsonAllocationLimit=4096;
    if(scenario=="restore-oom-candidate")jsonFailAt=2;
    if(scenario=="restore-oom-body")stringAllocationLimit=32;
    if(scenario=="restore-oom-concat")stringConcatLimit=1;
    assert(backupHandler(&req)==ESP_OK);
    const auto attempted=jsonAllocationCount;
    normalMemory();
    if(scenario=="restore-fit"){
      assert(req.status=="202 Accepted"&&req.body=="{\"code\":\"accepted\"}");
      assert(config["hub"]["devices"].size()==12&&hubService.user()=="newowner");
      assert(attempted>2&&deniedJsonAllocations==0);assertQuiesced(login.token,fd);assert(rebootAt);
    }else{
      assert(req.status=="503 Service Unavailable"&&req.body=="{\"code\":\"busy\"}");
      assert(prefs.persisted==before&&prefs.writes==writes&&!rebootAt);assertLive(login.token,fd);
      if(scenario=="restore-oom-parse")assert(attempted>1&&deniedJsonAllocations==1);
      if(scenario=="restore-oom-candidate")assert(attempted==2&&deniedJsonAllocations==1);
      if(scenario=="restore-oom-body")assert(attempted==0&&req.offset==0);
      if(scenario=="restore-oom-concat")assert(attempted==0&&req.offset>0);
    }
  }else if(scenario=="mqtt"||scenario=="mqtt-local"||scenario=="mqtt-fail"){
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
      for(const auto &message:hubMessages)assert(!message);
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
