static esp_err_t jsonResponse(httpd_req_t *req,const char *status,const String &body){
  httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");httpd_resp_set_status(req,status);httpd_resp_set_type(req,"application/msgpack");httpd_resp_set_hdr(req,"Cache-Control","no-store");
  httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
  if(body.startsWith("{")){
    String wire;
    if(body.length()<384){StaticJsonDocument<768> doc;if(deserializeJson(doc,body))return ESP_FAIL;if(!serializeWireMP(doc,wire)){httpd_resp_set_status(req,"503 Service Unavailable");return httpd_resp_send(req,"\x81\xa4" "code" "\xa4" "busy",11);}}
    else {DynamicJsonDocument doc(12288);if(deserializeJson(doc,body)){httpd_resp_set_status(req,"503 Service Unavailable");return httpd_resp_send(req,"\x81\xa4" "code" "\xa4" "busy",11);}if(!serializeWireMP(doc,wire)){httpd_resp_set_status(req,"503 Service Unavailable");return httpd_resp_send(req,"\x81\xa4" "code" "\xa4" "busy",11);}}
    return httpd_resp_send(req,wire.c_str(),wire.length());
  }
  return httpd_resp_send(req,body.c_str(),body.length());
}
static bool authorized(httpd_req_t *req);
// HTTP server owns these sessions. Credentials changing on the application
// task invalidate all tokens on the next request; tokens never enter config.
struct WebSession {String token;uint32_t created=0,seen=0;};
static WebSession webSessions[4];
static String sessionPassword;
static String currentWebPassword(){char password[33];portENTER_CRITICAL(&authMux);memcpy(password,webPassword,sizeof(password));portEXIT_CRITICAL(&authMux);return String(password);}
static void refreshWebSessions(){String password=currentWebPassword();if(password!=sessionPassword){for(auto &s:webSessions)s=WebSession();sessionPassword=password;}uint32_t now=millis();for(auto &s:webSessions)if(s.token.length()&&(uint32_t(now-s.seen)>=1800000||uint32_t(now-s.created)>=86400000))s=WebSession();}
static WebSession *bearerSession(httpd_req_t *req){refreshWebSessions();char header[80]={};if(httpd_req_get_hdr_value_str(req,"Authorization",header,sizeof(header))!=ESP_OK||strncmp(header,"Bearer ",7))return nullptr;for(auto &s:webSessions)if(s.token.length()&&s.token==header+7){s.seen=millis();return &s;}return nullptr;}
static bool authorized(httpd_req_t *req){if(bearerSession(req))return true;jsonResponse(req,"401 Unauthorized","{\"code\":\"unauthorized\"}");return false;}
static esp_err_t loginHandler(httpd_req_t *req){
  refreshWebSessions();char type[64]={};
  if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||strcmp(type,"application/msgpack"))return jsonResponse(req,"415 Unsupported Media Type","{\"code\":\"invalid_params\"}");
  if(req->content_len<=0||req->content_len>128)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  char body[128];int left=req->content_len,read=0;while(left){int n=httpd_req_recv(req,body+read,left);if(n<=0)return ESP_FAIL;read+=n;left-=n;}
  DynamicJsonDocument input(512);if(deserializeMsgPack(input,body,read)||!input.is<JsonObject>()||input.size()!=1||!input["password"].is<const char*>())return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  const char *password=input["password"];if(input["password"].as<JsonString>().size()!=strlen(password))return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");unsigned difference=strlen(password)^sessionPassword.length();for(size_t i=0;i<sessionPassword.length();i++)difference|=uint8_t(sessionPassword[i])^uint8_t(i<strlen(password)?password[i]:0);
  if(difference)return jsonResponse(req,"401 Unauthorized","{\"code\":\"unauthorized\"}");
  WebSession *slot=nullptr;for(auto &s:webSessions)if(!s.token.length()){slot=&s;break;}
  if(!slot)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");
  char token[33];for(unsigned i=0;i<4;i++)snprintf(token+i*8,9,"%08lx",(unsigned long)esp_random());
  slot->token=token;slot->created=slot->seen=millis();
  DynamicJsonDocument reply(1536);reply["code"]="ok";JsonObject data=reply.createNestedObject("data");data["token"]=slot->token;data["expires_in"]=1800;
  String result;serializeWireMP(reply,result);return jsonResponse(req,"200 OK",result);
}
static esp_err_t logoutHandler(httpd_req_t *req){auto session=bearerSession(req);if(!session)return jsonResponse(req,"401 Unauthorized","{\"code\":\"unauthorized\"}");*session=WebSession();return jsonResponse(req,"200 OK","{\"code\":\"ok\"}");}
// HTTP handlers run on the server task; application work is queued below.
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
  httpd_resp_set_type(req,"text/html; charset=utf-8");httpd_resp_set_hdr(req,"Content-Encoding","gzip");
  return httpd_resp_send(req,(const char*)web_data,sizeof(web_data));
}
static esp_err_t commandHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  if(webPending||!webDone)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\",\"message\":\"Command still pending\"}");
  if(req->content_len<=0||req->content_len>4096)return jsonResponse(req,"413 Payload Too Large","{\"code\":\"invalid_params\",\"message\":\"Command too large\"}");
  char type[64];if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||strcmp(type,"application/msgpack"))return jsonResponse(req,"415 Unsupported Media Type","{\"code\":\"invalid_params\",\"message\":\"Expected MessagePack\"}");
  String body;body.reserve(req->content_len);char buffer[513];int left=req->content_len;
  while(left){int n=httpd_req_recv(req,buffer,min(left,512));if(n<=0)return ESP_FAIL;buffer[n]=0;body.concat(buffer,n);left-=n;}
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
    DynamicJsonDocument out(config.memoryUsage()+1024);
    if(!out.capacity())return "{\"code\":\"busy\"}";
    out["schema"]=1;out["model"]="daikin";out["variant"]=DAIKIN_VARIANT;out["config_version"]=1;out["firmware_version"]=version;out["config"]=config;
    if(out.overflowed())return "{\"code\":\"busy\"}";
    const size_t length=measureJson(out);String text;
    if(!text.reserve(length))return "{\"code\":\"busy\"}";
    serializeJson(out,text);
    if(text.length()!=length)return "{\"code\":\"busy\"}";
    return text;
  }
  const size_t limit=configCapacity+1024;
  size_t capacity=config.memoryUsage()+1024;if(capacity>limit)capacity=limit;
  for(;;){
    // JSON structure and strings determine memory use. Retry a genuinely larger
    // document after NoMemory, releasing each previous pool before allocating.
    DynamicJsonDocument backup(capacity);
    if(!backup.capacity())return "{\"code\":\"busy\"}";
    auto error=deserializeJson(backup,webBody);
    if(error==DeserializationError::NoMemory){
      if(capacity==limit)return "{\"code\":\"busy\"}";
      capacity=capacity>limit-1024?limit:capacity+1024;continue;
    }
    if(error||!backup.is<JsonObject>()||backup["schema"]!=1||String(backup["model"]|"")!="daikin"||
      String(backup["variant"]|"")!=DAIKIN_VARIANT||backup["config_version"]!=1||!validConfig(backup["config"].as<JsonObjectConst>()))return "{\"code\":\"invalid_backup\"}";
    auto restored=backup["config"].as<JsonObjectConst>();
    size_t candidateCapacity=restored.memoryUsage()+1024;if(candidateCapacity>configCapacity)candidateCapacity=configCapacity;
    DynamicJsonDocument candidate(candidateCapacity);
    if(!candidate.capacity()||!candidate.set(restored)||candidate.overflowed())return "{\"code\":\"busy\"}";
    if(!saveConfig(candidate))return "{\"code\":\"storage_error\"}";
#ifdef IOTCLOUD_HUB
    hubRestoreChanged();
#endif
    rebootAt=millis()+1500;return "{\"code\":\"accepted\"}";
  }
}
static esp_err_t backupHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  bool passwordSet;portENTER_CRITICAL(&authMux);passwordSet=*webPassword;portEXIT_CRITICAL(&authMux);
  if(!passwordSet)return jsonResponse(req,"403 Forbidden","{\"code\":\"set_web_password_first\"}");
  if(webPending||!webDone)return jsonResponse(req,"409 Conflict","{\"code\":\"busy\"}");
  const bool restore=req->method==HTTP_POST;
  if(restore&&(req->content_len<=0||req->content_len>int(configCapacity+1024)))return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_backup\"}");
  String body;char buffer[513];int left=restore?req->content_len:0;
  if(restore&&!body.reserve(left))return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");
  while(left){int n=httpd_req_recv(req,buffer,min(left,512));if(n<=0)return ESP_FAIL;buffer[n]=0;if(!body.concat(buffer,n))return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");left-=n;}
  webBody=std::move(body);webOperation=restore?2:1;webDone=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  uint32_t started=millis();while(!webDone){if(uint32_t(millis()-started)>=10000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\"}");delay(1);}
  if(!restore&&webResult.startsWith("{\"schema\":")){
    httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");httpd_resp_set_hdr(req,"Cache-Control","no-store");httpd_resp_set_type(req,"application/octet-stream");return httpd_resp_send(req,webResult.c_str(),webResult.length());
  }
  if(webResult.startsWith("{\"code\":\"busy\""))return jsonResponse(req,"503 Service Unavailable",webResult);
  return jsonResponse(req,webResult.indexOf("accepted")>=0?"202 Accepted":"400 Bad Request",webResult);
}
static volatile bool otaActive=false;
struct OtaFlash {const esp_partition_t *partition;esp_ota_handle_t handle;size_t work;};
static bool otaErase(void *context){
  auto *f=(OtaFlash*)context;
  // Writes are continuous and bounded to 1024 bytes. Let IDF erase only newly
  // reached sectors instead of blocking the HTTP task on a whole-image erase.
  return esp_ota_begin(f->partition,OTA_WITH_SEQUENTIAL_WRITES,&f->handle)==ESP_OK;
}
static void otaYield(OtaFlash *f,size_t n){if((f->work+=n)>=16384){f->work=0;delay(1);}}
static bool otaWrite(void *context,uint32_t offset,const uint8_t *data,size_t n){
  (void)offset;auto *f=(OtaFlash*)context;
  if(esp_ota_write(f->handle,data,n)!=ESP_OK)return false;
  otaYield(f,n);return true;
}
static bool otaRead(void *context,uint32_t offset,uint8_t *data,size_t n){
  auto *f=(OtaFlash*)context;
  if(esp_partition_read(f->partition,offset,data,n)!=ESP_OK)return false;
  otaYield(f,n);return true;
}
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
  OtaFlash flash={partition,0,0};signed_ota verifier;
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
  while(ok&&left>0){int n=httpd_req_recv(req,(char*)buffer,min(left,512));if(n<=0){ok=false;break;}ok=signedImage?signed_ota_write(&verifier,buffer,n):otaWrite(&flash,0,buffer,n);left-=n;}
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
  DynamicJsonDocument reply(1536);reply["code"]="ok";JsonObject data=reply.createNestedObject("data");
  data["api_version"]=1;data["model"]="daikin";data["firmware_variant"]=DAIKIN_VARIANT;
  data["version"]=version;data["device_id"]=deviceId;data["mac"]=deviceId;
  data["api_version"]=1;data["model"]="daikin";data["variant"]=DAIKIN_VARIANT;data["device_id"]=deviceId;data["fw"]=version;data["firmware_version"]=version;
  data["auth"]="bearer";data["password_set"]=currentWebPassword().length()>0;data["auth_required"]=true;data["wire_version"]=1;data.createNestedArray("wire_formats").add("msgpack");data["default_wire_format"]="msgpack";
  String body;serializeWireMP(reply,body);return jsonResponse(req,"200 OK",body);
}
static esp_err_t statusHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  if(webPending||!webDone)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");
  webBody="";webOperation=4;webDone=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  uint32_t started=millis();while(!webDone){if(uint32_t(millis()-started)>=10000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\"}");delay(1);}
  return jsonResponse(req,webResult.startsWith("{\"code\":\"busy\"")?"503 Service Unavailable":"200 OK",webResult);
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
    // authorized() permits an unset Web password and verifies an existing one.
    setupAuthorized=true;
  }
  if(req->content_len>12288||req->content_len<0)return jsonResponse(req,"413 Payload Too Large","{\"code\":\"invalid_params\"}");
  if(!get&&req->method!=HTTP_DELETE){char type[64]={};if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||strcmp(type,"application/msgpack"))return jsonResponse(req,"415 Unsupported Media Type","{\"code\":\"invalid_params\"}");}
  String body;body.reserve(req->content_len);char buffer[513];int left=req->content_len;
  while(left){int n=httpd_req_recv(req,buffer,min(left,512));if(n<=0)return ESP_FAIL;buffer[n]=0;body.concat(buffer,n);left-=n;}
  if(body.length()){
    DynamicJsonDocument decoded(24576);if(deserializeMsgPack(decoded,body.c_str(),body.length())||!decoded.is<JsonObject>())return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
    String internal;serializeJson(decoded,internal);body=std::move(internal);
  }
  std::string token;
  if(httpd_req_get_hdr_value_str(req,"Authorization",authorization,sizeof(authorization))==ESP_OK&&!strncmp(authorization,"Bearer ",7))token=authorization+7;
  hubHttpRequest=iotcloud::HubService::Request(get?"GET":req->method==HTTP_POST?"POST":req->method==HTTP_PUT?"PUT":"DELETE",path.c_str(),std::string(body.c_str(),body.length()),token,hostname.c_str(),setupAuthorized);
  webOperation=3;webDone=false;hubHttpToken="";
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  uint32_t started=millis();while(!webDone){if(uint32_t(millis()-started)>=10000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\"}");delay(1);}
  const char *status=hubHttpStatus==200?"200 OK":hubHttpStatus==201?"201 Created":hubHttpStatus==202?"202 Accepted":hubHttpStatus==400?"400 Bad Request":hubHttpStatus==401?"401 Unauthorized":hubHttpStatus==403?"403 Forbidden":hubHttpStatus==404?"404 Not Found":hubHttpStatus==405?"405 Method Not Allowed":hubHttpStatus==409?"409 Conflict":hubHttpStatus==429?"429 Too Many Requests":"503 Service Unavailable";
  if(hubHttpToken.length())httpd_resp_set_hdr(req,"X-IotCloud-Session",hubHttpToken.c_str());
  return jsonResponse(req,status,webResult);
}
static void registerHubHttp(){
  if(!web)return;
  for(auto path:{"/iotcloud","/iotcloud/*"}){httpd_uri_t uri={};uri.uri=path;uri.method=HTTP_GET;uri.handler=hubAssetsHandler;httpd_register_uri_handler(web,&uri);}
  for(auto method:{HTTP_GET,HTTP_POST,HTTP_PUT,HTTP_DELETE,HTTP_OPTIONS}){httpd_uri_t uri={};uri.uri="/api/*";uri.method=method;uri.handler=hubApiHandler;httpd_register_uri_handler(web,&uri);}
}
#endif
