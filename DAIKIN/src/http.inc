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
  webBody=body;webDone=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  const uint32_t started=millis();
  while(!webDone){
    if(uint32_t(millis()-started)>=10000)return jsonResponse(req,"503 Service Unavailable","{\"code\":\"timeout\",\"message\":\"Command timed out; query state before retrying\"}");
    delay(1);
  }
  return jsonResponse(req,"200 OK",webResult);
}
static volatile bool otaActive=false;
struct OtaFlash {const esp_partition_t *partition;esp_ota_handle_t handle;size_t length;};
static bool otaErase(void *context){auto *f=(OtaFlash*)context;return esp_ota_begin(f->partition,f->length,&f->handle)==ESP_OK;}
static bool otaWrite(void *context,uint32_t offset,const uint8_t *data,size_t n){(void)offset;return esp_ota_write(((OtaFlash*)context)->handle,data,n)==ESP_OK;}
static bool otaRead(void *context,uint32_t offset,uint8_t *data,size_t n){return esp_partition_read(((OtaFlash*)context)->partition,offset,data,n)==ESP_OK;}
static bool receiveExact(httpd_req_t *req,uint8_t *data,size_t n){while(n){int used=httpd_req_recv(req,(char*)data,n);if(used<=0)return false;data+=used;n-=used;}return true;}
static esp_err_t uploadHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  const bool signedImage=strcmp(req->uri,"/api/v1/ota/upload-unsigned")!=0;
  const esp_partition_t *partition=esp_ota_get_next_update_partition(nullptr);
  const int envelope=signedImage?OTA_ENVELOPE_SIZE:0;
  if(!partition||req->content_len<320+envelope||req->content_len-envelope>(int)partition->size)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_image\",\"message\":\"Invalid image size\"}");
  otaActive=true;uint8_t buffer[1024];int left=req->content_len-envelope;
  OtaFlash flash={partition,0,(size_t)left};signed_ota verifier;
  bool ok=true;
  if(signedImage){
    signed_ota_io io={otaErase,otaWrite,otaRead,&flash};
    ok=signed_ota_begin(&verifier,req->content_len,partition->size,4,1,io)&&receiveExact(req,buffer,envelope)&&signed_ota_write(&verifier,buffer,envelope);
  }
  // Preserve the existing board descriptor so installed firmware accepts migration.
  const size_t prefix=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t)+sizeof(daikin_target);
  if(ok)ok=receiveExact(req,buffer,prefix);
  if(ok){
    auto *desc=(esp_app_desc_t*)(buffer+sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t));
    const esp_app_desc_t *self=esp_ota_get_app_description();
    ok=buffer[0]==0xe9&&desc->magic_word==ESP_APP_DESC_MAGIC_WORD&&!memcmp(desc->project_name,self->project_name,sizeof(desc->project_name))&&!memcmp(buffer+prefix-sizeof(daikin_target),daikin_target,sizeof(daikin_target));
  }
  if(ok&&!signedImage)ok=otaErase(&flash);
  if(ok){ok=signedImage?signed_ota_write(&verifier,buffer,prefix):otaWrite(&flash,0,buffer,prefix);left-=prefix;}
  while(ok&&left>0){int n=httpd_req_recv(req,(char*)buffer,min(left,(int)sizeof(buffer)));if(n<=0){ok=false;break;}ok=signedImage?signed_ota_write(&verifier,buffer,n):otaWrite(&flash,0,buffer,n);left-=n;}
  if(ok&&signedImage)ok=signed_ota_finish(&verifier);
  if(ok){ok=esp_ota_end(flash.handle)==ESP_OK;flash.handle=0;}
  if(flash.handle)esp_ota_abort(flash.handle);
  if(ok)ok=esp_ota_set_boot_partition(partition)==ESP_OK;
  otaActive=false;
  if(!ok)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_image\",\"message\":\"Signature, target, digest or flash validation failed\"}");
  prefs.putBool("ota_signed",signedImage);
  rebootAt=millis()+1500;
  return jsonResponse(req,"202 Accepted","{\"code\":\"accepted\",\"message\":\"Image verified; rebooting\"}");
}
static esp_err_t infoHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  return jsonResponse(req,"200 OK",String("{\"code\":\"ok\",\"data\":{\"api_version\":1,\"model\":\"daikin\",\"firmware_variant\":\"full\",\"version\":\"")+version+"\",\"device_id\":\""+deviceId+"\",\"mac\":\""+deviceId+"\"}}");
}
static esp_err_t optionsHandler(httpd_req_t *req){httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");httpd_resp_set_hdr(req,"Access-Control-Allow-Methods","GET, POST, OPTIONS");httpd_resp_set_hdr(req,"Access-Control-Allow-Headers","Content-Type, Authorization");return httpd_resp_send(req,nullptr,0);}
