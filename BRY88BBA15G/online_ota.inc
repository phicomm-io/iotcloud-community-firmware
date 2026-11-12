/* Bounded network OTA. Metadata uses openiot MessagePack; the signed image is
 * checked by the same verifier and fixed hardware descriptor as local upload. */
#include "../common/ota/release_manifest.h"
struct G3OnlineState {
  char state[24], error[48], source[8], version[32], url[256];
  uint32_t size; bool available, busy, install, success;
};
static G3OnlineState g3Online={"idle","","github","","",0,false,false,false,false};
static QueueHandle_t g3OnlineResults;
static const char *g3OnlineBase(const char *source){
  return !strcmp(source,"cn")?"http://phicomm.fjrcn.cn":!strcmp(source,"global")?"http://phicomm.fjrcn.com":"https://phicomm-io.github.io";
}
struct G3Http {
  WiFiClient plain; WiFiClientSecure tls; HTTPClient http;
  ~G3Http(){http.end();tls.stop();plain.stop();}
};
static G3Http *g3HttpOpen(const char *url,int64_t *size){
  auto client=new(std::nothrow) G3Http;if(!client)return nullptr;
  client->http.setConnectTimeout(10000);client->http.setTimeout(15000);client->http.setReuse(false);
  client->tls.setInsecure();
  bool secure=!strncmp(url,"https://",8);
  bool ok=secure?client->http.begin(client->tls,url):client->http.begin(client->plain,url);
  if(ok)ok=client->http.GET()==200;
  *size=client->http.getSize();
  if(!ok||*size<=0){delete client;return nullptr;}return client;
}
static bool g3HttpRead(G3Http *client,uint8_t *bytes,size_t count){
  while(count){size_t n=client->http.getStreamPtr()->readBytes(bytes,count);if(!n)return false;bytes+=n;count-=n;delay(1);}return true;
}
static void g3OnlineWorker(void *opaque){
  G3OnlineState result=*static_cast<G3OnlineState*>(opaque);bool ok=false;
  int64_t length=0;G3Http *client=nullptr;
  if(!result.install){
    char url[256];snprintf(url,sizeof(url),"%s/ota/%sbry88bba15g/full/latest.msgpack",g3OnlineBase(result.source),strstr(version,"-rc")?"candidate/":"v1/");
    client=g3HttpOpen(url,&length);
    if(client&&length<=4096){
      uint8_t *bytes=(uint8_t*)malloc(length);ph_release_manifest manifest;
      ok=bytes&&g3HttpRead(client,bytes,length)&&ph_release_manifest_parse(bytes,length,"bry88bba15g","full",&manifest);
      free(bytes);
      if(ok){
        ph_ota_version_t current,target;ok=ph_ota_version_parse(version,&current)&&ph_ota_version_parse(manifest.version,&target);
        if(ok){strlcpy(result.version,manifest.version,sizeof(result.version));result.size=manifest.size;
          snprintf(result.url,sizeof(result.url),"%s%s",g3OnlineBase(result.source),manifest.path);
          result.available=ph_ota_version_compare(&target,&current)>0;
          strlcpy(result.state,result.available?"available":"up_to_date",sizeof(result.state));}
      }
    }
  }else{
    const esp_partition_t *partition=esp_ota_get_next_update_partition(nullptr);
    OtaFlash flash={partition,0,0};signed_ota verifier;uint8_t buffer[1024];
    // The application owner must keep polling the UART/power acknowledgement.
    // Wait here in the worker, never in its command dispatcher.
    if(screenModulePowerBegin())client=g3HttpOpen(result.url,&length);
    const size_t prefix=daikin::firmwarePrefixSize,initial=OTA_ENVELOPE_SIZE+prefix;
    ok=client&&partition&&length==result.size&&length>(int64_t)initial&&length-OTA_ENVELOPE_SIZE<=partition->size;
    if(ok)ok=g3HttpRead(client,buffer,initial)&&daikin::matchingFirmwarePrefix(buffer+OTA_ENVELOPE_SIZE,prefix,daikin_target,esp_ota_get_app_description()->project_name);
    if(ok){signed_ota_io io={otaErase,otaWrite,otaRead,&flash};ok=signed_ota_begin(&verifier,length,partition->size,otaModel,1,io)&&signed_ota_write(&verifier,buffer,initial);}
    int64_t left=length-initial;const uint32_t started=millis();
    while(ok&&left>0){size_t n=min((int64_t)sizeof(buffer),left);
      ok=uint32_t(millis()-started)<300000&&screenModulePowerHealthy()&&g3HttpRead(client,buffer,n)&&signed_ota_write(&verifier,buffer,n);left-=n;}
    if(ok)ok=signed_ota_finish(&verifier)&&otaRead(&flash,0,buffer,prefix)&&daikin::matchingFirmwarePrefix(buffer,prefix,daikin_target,esp_ota_get_app_description()->project_name);
    if(ok){ok=esp_ota_end(flash.handle)==ESP_OK;flash.handle=0;}
    if(flash.handle)esp_ota_abort(flash.handle);
    if(ok)ok=esp_ota_set_boot_partition(partition)==ESP_OK;
    result.success=ok;strlcpy(result.state,ok?"rebooting":"error",sizeof(result.state));
  }
  delete client;
  if(!ok){strlcpy(result.state,"error",sizeof(result.state));strlcpy(result.error,result.install?"download_or_validation_failed":"manifest_unavailable",sizeof(result.error));result.available=false;}
  result.busy=false;xQueueSend(g3OnlineResults,&result,portMAX_DELAY);vTaskDelete(nullptr);
}
static void g3OnlinePoll(){
  G3OnlineState result;
  if(g3OnlineResults&&xQueueReceive(g3OnlineResults,&result,0)==pdTRUE){
    g3Online=result;g3OnlineBusy=false;
    if(result.install){otaActive=false;if(result.success){prefs.putBool("ota_signed",true);rebootAt=millis()+1500;}else screenModulePowerEnd();}
  }
}
static const char *g3OnlineCommand(const String &action,JsonObjectConst params,JsonObject out){
  if(action=="ota.sources.get"){out["default"]="github";out["channel"]=strstr(version,"-rc")?"candidate":"stable";auto a=out.createNestedArray("sources");for(auto s:{"github","cn","global"})a.add(s);return "ok";}
  if(action=="ota.check.get"||action=="ota.status.get"){
    out["state"]=g3Online.state;out["error"]=g3Online.error;out["version"]=g3Online.version;out["available"]=g3Online.available;out["busy"]=g3Online.busy;return "ok";
  }
  const bool install=action=="ota.install";
  if(!install&&action!="ota.check")return "unsupported";
  if(g3OnlineBusy||otaActive||screenUploadBusy||screenImage.active||screenFontBusy()||screenWallpaper.active||screenWallpaper.pending||screenWeatherBusy||rebootAt)return "busy";
  if(WiFi.status()!=WL_CONNECTED)return "offline";
  if(install&&(!g3Online.available||params.size()))return "not_ready";
  if(!install){const char *source=params["source"]|"";if(params.size()!=1||(strcmp(source,"github")&&strcmp(source,"cn")&&strcmp(source,"global")))return "invalid_params";strlcpy(g3Online.source,source,sizeof(g3Online.source));g3Online.available=false;}
  if(!g3OnlineResults)g3OnlineResults=xQueueCreate(1,sizeof(G3OnlineState));
  if(!g3OnlineResults||heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<24576)return "busy";
  g3OnlineBusy=true;g3Online.busy=true;g3Online.install=install;g3Online.error[0]=0;strlcpy(g3Online.state,install?"downloading":"checking",sizeof(g3Online.state));if(install)otaActive=true;
  if(xTaskCreate(g3OnlineWorker,"g3-online",8192,&g3Online,1,nullptr)!=pdPASS){g3Online.busy=g3OnlineBusy=false;otaActive=false;strlcpy(g3Online.state,"error",sizeof(g3Online.state));return "busy";}
  return "accepted";
}
