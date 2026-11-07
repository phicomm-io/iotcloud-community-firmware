/* Run the production network OTA owner with failed/partial transports and
 * verifier outcomes. Signature cryptography is covered by test_ota_identity.py. */
#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <cstdint>
#include "../../DAIKIN/include/firmware_identity.h"
#include "../../common/ota/signed_ota.h"
extern "C" {
#include "../../common/typed_wire.h"
}
using std::min;
struct String:std::string {using std::string::string;};
static size_t testStrlcpy(char *out,const char *in,size_t n){size_t len=strlen(in);if(n){memcpy(out,in,std::min(len,n-1));out[std::min(len,n-1)]=0;}return len;}
#define strlcpy testStrlcpy
static uint32_t now=100;static uint32_t millis(){return now;}static void delay(unsigned n){now+=n;}
static char version[32]="1.0.0-rc.5";
static bool g3OnlineBusy,otaActive,screenUploadBusy,screenWeatherBusy;
static struct {bool active;}screenImage;
static struct {bool active,pending;}screenWallpaper;
static bool screenFontBusy(){return false;}
static uint32_t rebootAt;
static int beginPower,endPower,activate,abortCount,endCount,verifyCount;
static bool powerHealthy=true,powerReady=true,signatureValid=true,taskAccepted=true,inWorker=false;
static bool screenModulePowerHealthy(){return powerHealthy;}
static bool screenModulePowerBegin(){assert(inWorker);beginPower++;return powerReady;}
static void screenModulePowerEnd(){endPower++;}
static struct {bool signedImage;void putBool(const char*,bool value){signedImage=value;}}prefs;
static const int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,WL_CONNECTED=3,ESP_OK=0,pdPASS=1,pdTRUE=1,portMAX_DELAY=0;
static size_t largest=48000;static size_t heap_caps_get_largest_free_block(int){return largest;}
static struct{int status(){return WL_CONNECTED;}}WiFi;
static void *pendingTask;
static int xTaskCreate(void(*)(void*),const char*,unsigned,void *arg,int,void*){pendingTask=arg;return taskAccepted?pdPASS:0;}
static void vTaskDelete(void*){}
static std::vector<uint8_t> queueBytes;
using QueueHandle_t=void*;
static QueueHandle_t xQueueCreate(unsigned,size_t size){queueBytes.resize(size);return queueBytes.data();}
static bool queued;
static int xQueueSend(QueueHandle_t,const void* value,int){memcpy(queueBytes.data(),value,queueBytes.size());queued=true;return pdTRUE;}
static int xQueueReceive(QueueHandle_t,void *value,int){if(!queued)return 0;queued=false;memcpy(value,queueBytes.data(),queueBytes.size());return pdTRUE;}
static std::vector<uint8_t> response,flash;
static size_t position,cutoff=SIZE_MAX;
static int httpStatus=200;static int64_t httpLength=-2;
static bool tlsInsecure;
static std::string requested;
struct WiFiClient {void stop(){}size_t readBytes(uint8_t *bytes,size_t count){size_t n=std::min(count,std::min(response.size()-position,cutoff>position?cutoff-position:0));memcpy(bytes,response.data()+position,n);position+=n;return n;}};
struct WiFiClientSecure:WiFiClient {void setInsecure(){tlsInsecure=true;}};
struct HTTPClient {
 WiFiClient stream;
 void end(){}void setConnectTimeout(int){}void setTimeout(int){}void setReuse(bool){}
 bool begin(WiFiClient&,const char *url){requested=url;position=0;return true;}
 int GET(){return httpStatus;}int64_t getSize(){return httpLength==-2?(int64_t)response.size():httpLength;}
 WiFiClient*getStreamPtr(){return &stream;}
};
struct esp_partition_t{uint32_t size;};static esp_partition_t partition={0x1e0000};
static const esp_partition_t *esp_ota_get_next_update_partition(void*){return &partition;}
static int esp_ota_set_boot_partition(const esp_partition_t*){activate++;return ESP_OK;}
static int esp_ota_end(unsigned){endCount++;return ESP_OK;}
static void esp_ota_abort(unsigned){abortCount++;}
struct App{char project_name[32];};static App app={"iotcloud-daikin"};
static const App *esp_ota_get_app_description(){return &app;}
static daikin::FirmwareIdentity daikin_target={"BRY88BBA15G","PHICOMM-DAIKIN-1:full"};
static const unsigned otaModel=6;
struct OtaFlash{const esp_partition_t* partition;unsigned handle,used;};
static bool otaErase(void *ctx){auto f=(OtaFlash*)ctx;f->handle=1;flash.clear();return true;}
static bool otaWrite(void*,uint32_t,const uint8_t*,size_t){return true;}
static bool otaRead(void*,uint32_t offset,uint8_t *out,size_t n){if(offset+n>flash.size())return false;memcpy(out,flash.data()+offset,n);return true;}
bool signed_ota_begin(signed_ota *v,uint32_t total,uint32_t maximum,uint16_t model,uint16_t hardware,signed_ota_io io){assert(model==6&&hardware==1&&total<=maximum+OTA_ENVELOPE_SIZE);memset(v,0,sizeof(*v));v->io=io;v->total=total;return true;}
bool signed_ota_write(signed_ota *v,const uint8_t *bytes,size_t n){
 if(!v->received){if(!v->io.erase(v->io.context))return false;assert(n>OTA_ENVELOPE_SIZE);bytes+=OTA_ENVELOPE_SIZE;n-=OTA_ENVELOPE_SIZE;v->received=OTA_ENVELOPE_SIZE;}
 flash.insert(flash.end(),bytes,bytes+n);v->received+=n;return true;
}
bool signed_ota_finish(signed_ota*v){verifyCount++;return signatureValid&&v->received==v->total;}
#include "../online_ota.inc"
static void command(const char*name,JsonObjectConst params,const char*expected){DynamicJsonDocument result(4096);assert(!strcmp(g3OnlineCommand(String(name),params,result.to<JsonObject>()),expected));}
static void runTask(){inWorker=true;g3OnlineWorker(pendingTask);inWorker=false;g3OnlinePoll();assert(!g3OnlineBusy&&!g3Online.busy);}
static void reset(){response.clear();flash.clear();requested.clear();cutoff=SIZE_MAX;httpLength=-2;httpStatus=200;activate=abortCount=endCount=verifyCount=beginPower=endPower=0;powerHealthy=powerReady=signatureValid=taskAccepted=true;inWorker=false;largest=48000;rebootAt=0;otaActive=g3OnlineBusy=false;g3Online={"idle","","github","","",0,false,false,false,false};}
static void image(){response.resize(OTA_ENVELOPE_SIZE+4096);auto p=response.data()+OTA_ENVELOPE_SIZE;p[0]=0xe9;const uint8_t magic[]={0x32,0x54,0xcd,0xab};memcpy(p+32,magic,4);memcpy(p+80,app.project_name,32);memcpy(p+288,&daikin_target,64);}
static void install(){g3Online.available=true;g3Online.size=response.size();strlcpy(g3Online.url,"https://example.test/release.signed",sizeof(g3Online.url));DynamicJsonDocument empty(64);command("ota.install",empty.to<JsonObject>(),"accepted");runTask();}
int main(){
 DynamicJsonDocument params(256);params["source"]="github";
 // Actual MessagePack metadata passes through the production reader.
 DynamicJsonDocument manifest(2048);manifest["schema"]=1;manifest["tag"]="v1.0.0-rc.6";manifest["version"]="1.0.0-rc.6";manifest["commit"]=std::string(40,'a');manifest["model"]="bry88bba15g";manifest["firmware_variant"]="full";manifest["path"]="/releases/v1.0.0-rc.6/daikin_BRY88BBA15G.ota.bin.signed";manifest["size"]=5000;manifest["sha256"]=std::string(64,'b');manifest["release_log"]="/releases/v1.0.0-rc.6/notes.md";
 reset();response.resize(measureMsgPack(manifest));serializeMsgPack(manifest,response.data(),response.size());command("ota.check",params.as<JsonObjectConst>(),"accepted");runTask();assert(g3Online.available&&!strcmp(g3Online.version,"1.0.0-rc.6")&&tlsInsecure);assert(requested.find("/ota/candidate/bry88bba15g/full/latest.msgpack")!=std::string::npos);assert(!activate&&!beginPower);
 reset();response={0xc0};command("ota.check",params.as<JsonObjectConst>(),"accepted");runTask();assert(!g3Online.available&&!strcmp(g3Online.state,"error"));
 for(int fault=0;fault<7;fault++){
  reset();image();
  if(fault==1)response[OTA_ENVELOPE_SIZE+288]='X';
  if(fault==2)cutoff=700;
  if(fault==3)signatureValid=false;
  if(fault==4)httpStatus=500;
  if(fault==5)powerHealthy=false;
  if(fault==6)powerReady=false;
  install();assert(beginPower==1);
  if(!fault){assert(activate==1&&endCount==1&&verifyCount==1&&prefs.signedImage&&rebootAt>millis());}
  else{assert(!activate&&endPower==1&&!g3Online.available&&!strcmp(g3Online.state,"error"));}
  if(fault==1||fault==4||fault==6)assert(flash.empty());
  if(fault==6)assert(requested.empty());
 }
 reset();largest=1000;command("ota.check",params.as<JsonObjectConst>(),"busy");assert(!g3OnlineBusy);
 reset();taskAccepted=false;image();g3Online.available=true;DynamicJsonDocument empty(64);command("ota.install",empty.to<JsonObject>(),"busy");assert(!otaActive&&!g3OnlineBusy&&!beginPower&&!endPower);
 puts("online OTA: worker-only power wait, prepare failure, metadata, wrong board, HTTP failure, truncation, verifier failure, power loss and heap/task rejection passed");
}
