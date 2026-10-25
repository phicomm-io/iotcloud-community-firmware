static esp_err_t jsonResponse(httpd_req_t *req,const char *status,const String &body){
  httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");httpd_resp_set_status(req,status);httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
  return httpd_resp_send(req,body.c_str(),body.length());
}
static bool authorized(httpd_req_t *req){
  char pass[33];portENTER_CRITICAL(&authMux);memcpy(pass,webPassword,sizeof(pass));portEXIT_CRITICAL(&authMux);if(!*pass)return true;
  char header[160];uint8_t decoded[100];size_t n=0;
  bool ok=httpd_req_get_hdr_value_str(req,"Authorization",header,sizeof(header))==ESP_OK&&strncmp(header,"Basic ",6)==0&&
    mbedtls_base64_decode(decoded,sizeof(decoded)-1,&n,(unsigned char*)header+6,strlen(header+6))==0;
  String expected=String("admin:")+pass;uint8_t diff=n^expected.length();
  if(ok)for(size_t i=0;i<n;i++)diff|=decoded[i]^(i<expected.length()?expected[i]:0);
  memset(decoded,0,sizeof(decoded));
  if(ok&&!diff)return true;
  httpd_resp_set_hdr(req,"WWW-Authenticate","Basic realm=\"DAIKIN\"");jsonResponse(req,"401 Unauthorized","{\"code\":\"unauthorized\",\"message\":\"Authentication required\"}");return false;
}
// HTTP handlers run in the server task; commands and hardware/config use loop().
static portMUX_TYPE webMux=portMUX_INITIALIZER_UNLOCKED;
static volatile bool webPending=false,webDone=true;
static String webBody,webResult;
static unsigned webOperation=0;
#ifdef IOTCLOUD_HUB
static iotcloud::HubService::Request hubHttpRequest("","");
static int hubHttpStatus=200;
static String hubHttpToken;
#endif
static esp_err_t rootHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  httpd_resp_set_type(req,"text/html; charset=utf-8");httpd_resp_set_hdr(req,"Content-Encoding","gzip");
  return httpd_resp_send(req,(const char*)web_data,sizeof(web_data));
}
static esp_err_t commandHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  if(webPending||!webDone)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\",\"message\":\"Command still pending\"}");
  if(req->content_len<=0||req->content_len>4096)return jsonResponse(req,"413 Payload Too Large","{\"code\":\"invalid_params\",\"message\":\"Command too large\"}");
  char type[64];if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||strncmp(type,"application/json",16))return jsonResponse(req,"415 Unsupported Media Type","{\"code\":\"invalid_params\",\"message\":\"Expected JSON\"}");
  String body;body.reserve(req->content_len);char buffer[512];int left=req->content_len;
  while(left){int n=httpd_req_recv(req,buffer,min(left,(int)sizeof(buffer)));if(n<=0)return ESP_FAIL;body.concat(buffer,n);left-=n;}
  webOperation=0;webBody=body;webDone=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  const uint32_t started=millis();
  while(!webDone){
    if(uint32_t(millis()-started)>=10000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\",\"message\":\"Command timed out; query state before retrying\"}");
    delay(1);
  }
  return jsonResponse(req,"200 OK",webResult);
}
static String backupExecute(){
#ifdef IOTCLOUD_HUB
  if(hubRestartPending())return "{\"code\":\"restart_required\"}";
#endif
  if(configRecovery)return "{\"code\":\"migration_required\"}";
  if(webOperation==1){
    DynamicJsonDocument out(configCapacity+1024);
    out["schema"]=1;out["model"]="daikin";out["variant"]=DAIKIN_VARIANT;out["config_version"]=1;out["firmware_version"]=version;out["config"]=config;
    String text;serializeJson(out,text);return text;
  }
  DynamicJsonDocument backup(configCapacity+1024);
  if(deserializeJson(backup,webBody)||!backup.is<JsonObject>()||backup["schema"]!=1||String(backup["model"]|"")!="daikin"||
    String(backup["variant"]|"")!=DAIKIN_VARIANT||backup["config_version"]!=1||!validConfig(backup["config"].as<JsonObjectConst>()))return "{\"code\":\"invalid_backup\"}";
  DynamicJsonDocument candidate(configCapacity);candidate.set(backup["config"]);
  if(!saveConfig(candidate))return "{\"code\":\"storage_error\"}";
#ifdef IOTCLOUD_HUB
  hubRestoreChanged();
#endif
  rebootAt=millis()+1500;return "{\"code\":\"accepted\"}";
}
static esp_err_t backupHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  bool passwordSet;portENTER_CRITICAL(&authMux);passwordSet=*webPassword;portEXIT_CRITICAL(&authMux);
  if(!passwordSet)return jsonResponse(req,"403 Forbidden","{\"code\":\"set_web_password_first\"}");
  if(webPending||!webDone)return jsonResponse(req,"409 Conflict","{\"code\":\"busy\"}");
  const bool restore=req->method==HTTP_POST;
  if(restore&&(req->content_len<=0||req->content_len>int(configCapacity+1024)))return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_backup\"}");
  String body;char buffer[512];int left=restore?req->content_len:0;
  while(left){int n=httpd_req_recv(req,buffer,min(left,(int)sizeof(buffer)));if(n<=0)return ESP_FAIL;body.concat(buffer,n);left-=n;}
  webBody=body;webOperation=restore?2:1;webDone=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  uint32_t started=millis();while(!webDone){if(uint32_t(millis()-started)>=10000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\"}");delay(1);}
  if(!restore&&webResult.startsWith("{\"schema\":")){
    httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");httpd_resp_set_hdr(req,"Cache-Control","no-store");httpd_resp_set_type(req,"application/octet-stream");return httpd_resp_send(req,webResult.c_str(),webResult.length());
  }
  return jsonResponse(req,webResult.indexOf("accepted")>=0?"202 Accepted":"400 Bad Request",webResult);
}
static volatile bool otaActive=false;
struct OtaFlash {const esp_partition_t *partition;esp_ota_handle_t handle;size_t length;};
static bool otaErase(void *context){auto *f=(OtaFlash*)context;return esp_ota_begin(f->partition,f->length,&f->handle)==ESP_OK;}
static bool otaWrite(void *context,uint32_t offset,const uint8_t *data,size_t n){(void)offset;return esp_ota_write(((OtaFlash*)context)->handle,data,n)==ESP_OK;}
static bool otaRead(void *context,uint32_t offset,uint8_t *data,size_t n){return esp_partition_read(((OtaFlash*)context)->partition,offset,data,n)==ESP_OK;}
static bool receiveExact(httpd_req_t *req,uint8_t *data,size_t n){while(n){int used=httpd_req_recv(req,(char*)data,n);if(used<=0)return false;data+=used;n-=used;}return true;}
static esp_err_t uploadFirmware(httpd_req_t *req,bool signedImage,const daikin::FirmwareIdentity &expected){
  const esp_partition_t *partition=esp_ota_get_next_update_partition(nullptr);
  const int envelope=signedImage?OTA_ENVELOPE_SIZE:0;
  const size_t prefix=daikin::firmwarePrefixSize;
  static_assert(sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)==daikin::firmwareDescriptorOffset,"Unexpected ESP image header layout");
  static_assert(sizeof(esp_app_desc_t)==256,"Unexpected ESP app descriptor layout");
  if(!partition||req->content_len<(int)prefix+envelope||req->content_len-envelope>(int)partition->size)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_image\",\"message\":\"Invalid image size\"}");
  otaActive=true;uint8_t buffer[1024];int left=req->content_len-envelope;
  static_assert(sizeof(buffer)>=OTA_ENVELOPE_SIZE+daikin::firmwarePrefixSize,"OTA prefix buffer too small");
  OtaFlash flash={partition,0,(size_t)left};signed_ota verifier;
  // Buffer the bounded envelope and prefix before the verifier can erase the
  // inactive slot. This is only an identity prefilter; signature and complete
  // staged-payload digest validation remain mandatory for every signed image.
  bool ok=receiveExact(req,buffer,envelope+prefix);
  const esp_app_desc_t *self=esp_ota_get_app_description();
  if(ok)ok=daikin::matchingFirmwarePrefix(buffer+envelope,prefix,expected,self->project_name);
  if(ok&&signedImage){
    signed_ota_io io={otaErase,otaWrite,otaRead,&flash};
    ok=signed_ota_begin(&verifier,req->content_len,partition->size,4,1,io)&&signed_ota_write(&verifier,buffer,envelope);
  }
  if(ok&&!signedImage)ok=otaErase(&flash);
  if(ok){ok=signedImage?signed_ota_write(&verifier,buffer+envelope,prefix):otaWrite(&flash,0,buffer,prefix);left-=prefix;}
  while(ok&&left>0){int n=httpd_req_recv(req,(char*)buffer,min(left,(int)sizeof(buffer)));if(n<=0){ok=false;break;}ok=signedImage?signed_ota_write(&verifier,buffer,n):otaWrite(&flash,0,buffer,n);left-=n;}
  if(ok&&signedImage)ok=signed_ota_finish(&verifier);
  // Verify the actual staged identity again after signature/digest validation.
  if(ok)ok=otaRead(&flash,0,buffer,prefix)&&daikin::matchingFirmwarePrefix(buffer,prefix,expected,self->project_name);
  if(ok){ok=esp_ota_end(flash.handle)==ESP_OK;flash.handle=0;}
  if(flash.handle)esp_ota_abort(flash.handle);
  if(ok)ok=esp_ota_set_boot_partition(partition)==ESP_OK;
  otaActive=false;
  if(!ok)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_image\",\"message\":\"Signature, target, digest or flash validation failed\"}");
  prefs.putBool("ota_signed",signedImage);
  rebootAt=millis()+1500;
  return jsonResponse(req,"202 Accepted","{\"code\":\"accepted\",\"message\":\"Image verified; rebooting\"}");
}
static esp_err_t uploadHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  // Confirmation headers never change either ordinary upload route's target.
  return uploadFirmware(req,strcmp(req->uri,"/api/v1/ota/upload-unsigned")!=0,daikin_target);
}
#ifndef IOTCLOUD_HUB
static esp_err_t convertHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  bool passwordSet;portENTER_CRITICAL(&authMux);passwordSet=*webPassword;portEXIT_CRITICAL(&authMux);
  if(!passwordSet)return jsonResponse(req,"403 Forbidden","{\"code\":\"set_web_password_first\"}");
  if(configRecovery)return jsonResponse(req,"409 Conflict","{\"code\":\"migration_required\"}");
  if(otaActive||webPending||!webDone||rebootAt)return jsonResponse(req,"409 Conflict","{\"code\":\"busy\"}");
  char target[8]={},confirm[16]={};
  if(httpd_req_get_hdr_value_str(req,"X-IoTCloud-Target-Variant",target,sizeof(target))!=ESP_OK||strcmp(target,"hub")||
     httpd_req_get_hdr_value_str(req,"X-IoTCloud-Confirm",confirm,sizeof(confirm))!=ESP_OK||strcmp(confirm,"full-to-hub"))
    return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  daikin::FirmwareIdentity expected;
  if(!daikin::fullToHubIdentity(daikin_target,expected))return jsonResponse(req,"409 Conflict","{\"code\":\"unsupported_conversion\"}");
  return uploadFirmware(req,true,expected);
}
#endif
static esp_err_t infoHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  DynamicJsonDocument reply(512);reply["code"]="ok";JsonObject data=reply.createNestedObject("data");
  data["api_version"]=1;data["model"]="daikin";data["firmware_variant"]=DAIKIN_VARIANT;
  data["version"]=version;data["device_id"]=deviceId;data["mac"]=deviceId;
  String body;serializeJson(reply,body);return jsonResponse(req,"200 OK",body);
}
static esp_err_t statusHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  if(webPending||!webDone)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");
  webBody="";webOperation=4;webDone=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  uint32_t started=millis();while(!webDone){if(uint32_t(millis()-started)>=10000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\"}");delay(1);}
  return jsonResponse(req,"200 OK",webResult);
}
static esp_err_t optionsHandler(httpd_req_t *req){httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");httpd_resp_set_hdr(req,"Access-Control-Allow-Methods","GET, POST, OPTIONS");httpd_resp_set_hdr(req,"Access-Control-Allow-Headers","Content-Type, Authorization, X-IoTCloud-Target-Variant, X-IoTCloud-Confirm");return httpd_resp_send(req,nullptr,0);}

#ifdef IOTCLOUD_HUB
static esp_err_t hubAssetsHandler(httpd_req_t *req){
  String path=req->uri;int query=path.indexOf('?');if(query>=0)path=path.substring(0,query);
  if(path=="/iotcloud"||path=="/iotcloud/")path="/iotcloud/index.html";
  for(const auto &asset:hub_assets)if(path==asset.path){
    httpd_resp_set_type(req,asset.type);httpd_resp_set_hdr(req,"Content-Encoding","gzip");httpd_resp_set_hdr(req,"Cache-Control","no-cache");
    return httpd_resp_send(req,(const char*)asset.data,asset.size);
  }
  httpd_resp_set_status(req,"404 Not Found");return httpd_resp_send(req,nullptr,0);
}
static esp_err_t hubApiHandler(httpd_req_t *req){
  if(req->method==HTTP_OPTIONS){httpd_resp_set_status(req,"403 Forbidden");return httpd_resp_send(req,nullptr,0);}
  if(webPending||!webDone)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");
  char host[96]={},origin[128]={},authorization[100]={};
  if(httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))!=ESP_OK)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  String hostname=host;int colon=hostname.indexOf(':');if(colon>=0)hostname=hostname.substring(0,colon);
  if(hostname!=WiFi.localIP().toString()&&hostname!=WiFi.softAPIP().toString()&&hostname!="daikin-"+deviceId+".local")return jsonResponse(req,"403 Forbidden","{\"code\":\"unauthorized\"}");
  bool get=req->method==HTTP_GET;
  if(!get&&httpd_req_get_hdr_value_len(req,"Origin")&&
    (httpd_req_get_hdr_value_str(req,"Origin",origin,sizeof(origin))!=ESP_OK||String(origin)!="http://"+String(host)))return jsonResponse(req,"403 Forbidden","{\"code\":\"unauthorized\"}");
  String path=req->uri;if(path.indexOf('?')>=0)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  bool setupAuthorized=false;
  if(path=="/api/hub/setup"){
    if(!authorized(req))return ESP_OK;
    portENTER_CRITICAL(&authMux);setupAuthorized=*webPassword;portEXIT_CRITICAL(&authMux);
  }
  if(req->content_len>12288||req->content_len<0)return jsonResponse(req,"413 Payload Too Large","{\"code\":\"invalid_params\"}");
  if(!get&&req->method!=HTTP_DELETE){char type[64]={};if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||strncmp(type,"application/json",16))return jsonResponse(req,"415 Unsupported Media Type","{\"code\":\"invalid_params\"}");}
  String body;body.reserve(req->content_len);char buffer[512];int left=req->content_len;
  while(left){int n=httpd_req_recv(req,buffer,min(left,(int)sizeof(buffer)));if(n<=0)return ESP_FAIL;body.concat(buffer,n);left-=n;}
  std::string token;
  if(httpd_req_get_hdr_value_str(req,"Authorization",authorization,sizeof(authorization))==ESP_OK&&!strncmp(authorization,"Bearer ",7))token=authorization+7;
  hubHttpRequest=iotcloud::HubService::Request(get?"GET":req->method==HTTP_POST?"POST":req->method==HTTP_PUT?"PUT":"DELETE",path.c_str(),body.c_str(),token,hostname.c_str(),setupAuthorized);
  webOperation=3;webDone=false;hubHttpToken="";
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  uint32_t started=millis();while(!webDone){if(uint32_t(millis()-started)>=10000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\"}");delay(1);}
  const char *status=hubHttpStatus==200?"200 OK":hubHttpStatus==201?"201 Created":hubHttpStatus==202?"202 Accepted":hubHttpStatus==400?"400 Bad Request":hubHttpStatus==401?"401 Unauthorized":hubHttpStatus==403?"403 Forbidden":hubHttpStatus==404?"404 Not Found":hubHttpStatus==405?"405 Method Not Allowed":hubHttpStatus==409?"409 Conflict":hubHttpStatus==429?"429 Too Many Requests":"503 Service Unavailable";
  if(hubHttpToken.length())httpd_resp_set_hdr(req,"X-IotCloud-Session",hubHttpToken.c_str());
  httpd_resp_set_status(req,status);httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
  return httpd_resp_send(req,webResult.c_str(),webResult.length());
}
static void registerHubHttp(){
  if(!web)return;
  for(auto path:{"/iotcloud","/iotcloud/*"}){httpd_uri_t uri={};uri.uri=path;uri.method=HTTP_GET;uri.handler=hubAssetsHandler;httpd_register_uri_handler(web,&uri);}
  for(auto method:{HTTP_GET,HTTP_POST,HTTP_PUT,HTTP_DELETE,HTTP_OPTIONS}){httpd_uri_t uri={};uri.uri="/api/*";uri.method=method;uri.handler=hubApiHandler;httpd_register_uri_handler(web,&uri);}
}
#endif
