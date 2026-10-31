static esp_err_t jsonResponse(httpd_req_t *req,const char *status,const String &body){
  httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");httpd_resp_set_status(req,status);httpd_resp_set_type(req,"application/msgpack");httpd_resp_set_hdr(req,"Cache-Control","no-store");
  httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
  if(body.startsWith("{")||body.startsWith("[")){
    String wire;
    if(body.length()<384){StaticJsonDocument<768> doc;if(deserializeJson(doc,body.c_str(),body.length()))return ESP_FAIL;if(!serializeWireMP(doc,wire)){httpd_resp_set_status(req,"503 Service Unavailable");return httpd_resp_send(req,"\x81\xa4" "code" "\xa4" "busy",11);}}
    else {DynamicJsonDocument doc(12288);if(deserializeJson(doc,body.c_str(),body.length())){httpd_resp_set_status(req,"503 Service Unavailable");return httpd_resp_send(req,"\x81\xa4" "code" "\xa4" "busy",11);}if(!serializeWireMP(doc,wire)){httpd_resp_set_status(req,"503 Service Unavailable");return httpd_resp_send(req,"\x81\xa4" "code" "\xa4" "busy",11);}}
    return httpd_resp_send(req,wire.c_str(),wire.length());
  }
  return httpd_resp_send(req,body.c_str(),body.length());
}
static bool authorized(httpd_req_t *req);
// HTTP and LAN share the credential epoch; restore revokes even unchanged passwords.
static String currentWebPassword(){char password[33];portENTER_CRITICAL(&authMux);memcpy(password,webPassword,sizeof(password));portEXIT_CRITICAL(&authMux);return String(password);}
static bool requestAuthorization(httpd_req_t *req,char header[80]){return httpd_req_get_hdr_value_str(req,"Authorization",header,80)==ESP_OK;}
static bool authorized(httpd_req_t *req){char header[80]={};ph_auth_context context;if(!configRecovery&&localAuthReady&&(requestAuthorization(req,header),ph_auth_http_check(&localAuth,header,&context)))return true;jsonResponse(req,"401 Unauthorized","{\"code\":\"unauthorized\"}");return false;}
static esp_err_t loginHandler(httpd_req_t *req){
  char type[64]={};
  if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||strcmp(type,"application/msgpack"))return jsonResponse(req,"415 Unsupported Media Type","{\"code\":\"invalid_params\"}");
  if(req->content_len<=0||req->content_len>128)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  char body[128];int left=req->content_len,read=0;while(left){int n=httpd_req_recv(req,body+read,left);if(n<=0)return ESP_FAIL;read+=n;left-=n;}
  ph_value root,value;char password[33]={};if(!phv_parse(PH_WIRE_MSGPACK,body,read,&root)||!phv_keys(&root,"|password||username|")||
     (phv_get(&root,"username",&value)&&!phv_equal_text(&value,"admin"))||!phv_get(&root,"password",&value)||!phv_text(&value,password,sizeof(password)))return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  char token[33]={};auto result=localAuthReady&&!configRecovery?ph_auth_http_login_result(&localAuth,password,token):PH_AUTH_LOGIN_BUSY;memset(password,0,sizeof(password));
  if(result!=PH_AUTH_LOGIN_OK)return jsonResponse(req,result==PH_AUTH_LOGIN_BUSY?"503 Service Unavailable":"401 Unauthorized",result==PH_AUTH_LOGIN_BUSY?"{\"code\":\"busy\"}":"{\"code\":\"unauthorized\"}");
  if(*token)httpd_resp_set_hdr(req,"X-IotCloud-Session",token);
  // The login result owns this decision. A later password change must not
  // turn a stateless result into a purported authenticated session.
  DynamicJsonDocument reply(768);reply["code"]="ok";reply["data"]["token"]=token;reply["data"]["password_required"]=*token!=0;reply["data"]["expires_in"]=1800;String wire;serializeWireMP(reply,wire);
  // IDF borrows the token header until httpd_resp_send finishes.
  const esp_err_t sent=jsonResponse(req,"200 OK",wire);
  memset(token,0,sizeof(token));return sent;
}
static esp_err_t logoutHandler(httpd_req_t *req){
  char header[80]={};ph_auth_context context;
  requestAuthorization(req,header);
  if(configRecovery||!localAuthReady||!ph_auth_http_check(&localAuth,header,&context))return jsonResponse(req,"401 Unauthorized","{\"code\":\"unauthorized\"}");
  if(context.source==PH_AUTH_HTTP&&!ph_auth_http_logout(&localAuth,header))return jsonResponse(req,"401 Unauthorized","{\"code\":\"unauthorized\"}");
  return jsonResponse(req,"200 OK","{\"code\":\"ok\"}");
}
// HTTP handlers run on the server task; application work is queued below.
static portMUX_TYPE webMux=portMUX_INITIALIZER_UNLOCKED;
static volatile bool webPending=false,webDone=true;
static volatile bool otaActive=false;
static String webBody,webResult;
static unsigned webOperation=0;
static ph_auth_context webAuthContext;
static bool webAuthRequired=false;
static bool captureWebAuthorization(httpd_req_t *req){char header[80]={};if(configRecovery||!localAuthReady||!(requestAuthorization(req,header),ph_auth_http_check(&localAuth,header,&webAuthContext))){jsonResponse(req,"401 Unauthorized","{\"code\":\"unauthorized\"}");return false;}webAuthRequired=true;return true;}
#ifdef IOTCLOUD_HUB
static iotcloud::HubService::Request hubHttpRequest("","");
static int hubHttpStatus=200;
static bool hubHttpDeferred=false;
static iotcloud::HubHttpCommand hubHttpCommand;
#endif
static esp_err_t rootHandler(httpd_req_t *req){
  httpd_resp_set_type(req,"text/html; charset=utf-8");httpd_resp_set_hdr(req,"Content-Encoding","gzip");
  return httpd_resp_send(req,(const char*)web_data,sizeof(web_data));
}
// A strict borrowed parameter view selects only two public, side-effect-free
// reads. The owned response is complete before the request buffer can expire.
static bool publicReadCommand(const String &body,String &response){
  ph_value root,action,params,id;char requestId[65]={};
  if(!phv_parse(PH_WIRE_MSGPACK,body.c_str(),body.length(),&root)||
     !phv_keys(&root,"|request_id||action||params|")||
     !phv_get(&root,"action",&action)||
     (!phv_equal_text(&action,"status.get")&&!phv_equal_text(&action,"config.get"))||
     !phv_get(&root,"params",&params)||params.kind!=PHV_MAP||!phv_keys(&params,""))return false;
  if(phv_get(&root,"request_id",&id)&&(!phv_text(&id,requestId,sizeof(requestId))||!requestId[0]))return false;
  auto busy=[&](){StaticJsonDocument<256> error;error["code"]="busy";if(*requestId)error["request_id"]=requestId;response="";serializeJson(error,response);};
  String data;
  if(!businessReadSnapshot.copy(data,false,millis())){busy();return true;}
  ph_writer writer;
  if(!phw_init_capacity(&writer,PH_WIRE_MSGPACK,data.length()+160)){busy();return true;}
  ph_buf cached={const_cast<char*>(data.c_str()),data.length(),data.length()+1};
  phw_begin(&writer,nullptr,false);if(*requestId)phw_text(&writer,"request_id",requestId);
  phw_text(&writer,"code","ok");phw_text(&writer,"message","ok");
  phw_cached_value(&writer,"data",&cached,8);phw_end(&writer);
  ph_buf *wire=phw_take(&writer);
  if(!wire||!response.reserve(wire->len)||!response.concat(wire->buf,wire->len))busy();
  ph_buf_free(wire);return true;
}
static esp_err_t commandHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  if(req->content_len<=0||req->content_len>4096)return jsonResponse(req,"413 Payload Too Large","{\"code\":\"invalid_params\",\"message\":\"Command too large\"}");
  char type[64];if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||strcmp(type,"application/msgpack"))return jsonResponse(req,"415 Unsupported Media Type","{\"code\":\"invalid_params\",\"message\":\"Expected MessagePack\"}");
  String body;if(!body.reserve(req->content_len))return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");char buffer[513];int left=req->content_len;
  while(left){int n=httpd_req_recv(req,buffer,min(left,512));if(n<=0)return ESP_FAIL;buffer[n]=0;if(!body.concat(buffer,n))return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");left-=n;}
  String response;
  if(!authorized(req))return ESP_OK; // Recheck after receiving the body.
  if(!otaActive&&publicReadCommand(body,response))return jsonResponse(req,response.startsWith("{")?"503 Service Unavailable":"200 OK",response);
  if(webPending||!webDone)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\",\"message\":\"Command still pending\"}");
  if(!captureWebAuthorization(req))return ESP_OK;
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
    auto error=deserializeJson(backup,webBody.c_str(),webBody.length());
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
    if(!saveConfigTransaction(candidate,true))return "{\"code\":\"storage_error\"}";
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
  if(!captureWebAuthorization(req))return ESP_OK;
  webBody=std::move(body);webOperation=restore?2:1;webDone=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  uint32_t started=millis();while(!webDone){if(uint32_t(millis()-started)>=10000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\"}");delay(1);}
  if(!restore&&webResult.startsWith("{\"schema\":")){
    httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");httpd_resp_set_hdr(req,"Cache-Control","no-store");httpd_resp_set_type(req,"application/octet-stream");return httpd_resp_send(req,webResult.c_str(),webResult.length());
  }
  if(webResult.startsWith("{\"code\":\"busy\""))return jsonResponse(req,"503 Service Unavailable",webResult);
  return jsonResponse(req,webResult.indexOf("accepted")>=0?"202 Accepted":"400 Bad Request",webResult);
}
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
  data["auth"]="bearer";data["password_set"]=currentWebPassword().length()>0;data["auth_required"]=currentWebPassword().length()>0;data["wire_version"]=1;data.createNestedArray("wire_formats").add("msgpack");data["default_wire_format"]="msgpack";
  String body;serializeWireMP(reply,body);return jsonResponse(req,"200 OK",body);
}
static esp_err_t statusHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  String response;
  if(otaActive||!businessReadSnapshot.copy(response,true,millis()))return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");
  return jsonResponse(req,"200 OK",response);
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
  char host[96]={},origin[128]={};
  if(httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))!=ESP_OK)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  String hostname=host;int colon=hostname.indexOf(':');if(colon>=0)hostname=hostname.substring(0,colon);
  if(hostname!=WiFi.localIP().toString()&&hostname!=WiFi.softAPIP().toString()&&hostname!=deviceHostname+".local")return jsonResponse(req,"403 Forbidden","{\"code\":\"unauthorized\"}");
  bool get=req->method==HTTP_GET;
  if(!get&&httpd_req_get_hdr_value_len(req,"Origin")&&
    (httpd_req_get_hdr_value_str(req,"Origin",origin,sizeof(origin))!=ESP_OK||String(origin)!="http://"+String(host)))return jsonResponse(req,"403 Forbidden","{\"code\":\"unauthorized\"}");
  String path=req->uri;if(path.indexOf('?')>=0)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  if(path=="/api/auth/login"&&req->method==HTTP_POST)return loginHandler(req);
  if(path=="/api/auth/logout"&&req->method==HTTP_POST)return logoutHandler(req);
  if(!authorized(req))return ESP_OK;
  const bool deviceCommand=path.startsWith("/api/hub/devices/");
  if(req->content_len>(deviceCommand?int(LAN_V4_REQUEST_MAX):12288)||req->content_len<0)return jsonResponse(req,"413 Payload Too Large","{\"code\":\"invalid_params\"}");
  if(!get&&req->method!=HTTP_DELETE){char type[64]={};if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||strcmp(type,"application/msgpack"))return jsonResponse(req,"415 Unsupported Media Type","{\"code\":\"invalid_params\"}");}
  String body;if(!body.reserve(req->content_len))return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");char buffer[513];int left=req->content_len;
  while(left){int n=httpd_req_recv(req,buffer,min(left,512));if(n<=0)return ESP_FAIL;buffer[n]=0;if(!body.concat(buffer,n))return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");left-=n;}
  // Device commands retain the exact binary contract, including binary values
  // and duplicate-field rejection by the common typed-wire parser.
  if(body.length()&&!deviceCommand){
    DynamicJsonDocument decoded(24576);if(deserializeMsgPack(decoded,body.c_str(),body.length())||!decoded.is<JsonObject>())return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
    String internal;serializeJson(decoded,internal);body=std::move(internal);
  }
  if(!captureWebAuthorization(req))return ESP_OK;
  hubHttpRequest=iotcloud::HubService::Request(get?"GET":req->method==HTTP_POST?"POST":req->method==HTTP_PUT?"PUT":"DELETE",path.c_str(),std::string(body.c_str(),body.length()),true,hostname.c_str());
  webOperation=3;webDone=false;hubHttpDeferred=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  uint32_t started=millis();while(!webDone){if(uint32_t(millis()-started)>=12000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\"}");delay(1);}
  if(!localAuthContextValid(&webAuthContext))return jsonResponse(req,"401 Unauthorized","{\"code\":\"unauthorized\"}");
  const char *status=hubHttpStatus==200?"200 OK":hubHttpStatus==201?"201 Created":hubHttpStatus==202?"202 Accepted":hubHttpStatus==400?"400 Bad Request":hubHttpStatus==401?"401 Unauthorized":hubHttpStatus==403?"403 Forbidden":hubHttpStatus==404?"404 Not Found":hubHttpStatus==405?"405 Method Not Allowed":hubHttpStatus==409?"409 Conflict":hubHttpStatus==429?"429 Too Many Requests":"503 Service Unavailable";
  return jsonResponse(req,status,webResult);
}
static void registerHubHttp(){
  if(!web)return;
  for(auto path:{"/iotcloud","/iotcloud/*"}){httpd_uri_t uri={};uri.uri=path;uri.method=HTTP_GET;uri.handler=hubAssetsHandler;httpd_register_uri_handler(web,&uri);}
  for(auto method:{HTTP_GET,HTTP_POST,HTTP_PUT,HTTP_DELETE,HTTP_OPTIONS}){httpd_uri_t uri={};uri.uri="/api/*";uri.method=method;uri.handler=hubApiHandler;httpd_register_uri_handler(web,&uri);}
}
#endif
