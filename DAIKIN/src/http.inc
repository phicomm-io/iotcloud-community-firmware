static esp_err_t jsonResponse(httpd_req_t *req,const char *status,const String &body){
  httpd_resp_set_status(req,status);httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
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
static esp_err_t uploadHandler(httpd_req_t *req){
  if(!authorized(req))return ESP_OK;
  if(strcmp(req->uri,"/api/v1/ota/upload-unsigned"))return jsonResponse(req,"400 Bad Request","{\"code\":\"unsupported\",\"message\":\"Use explicit unsigned ESP32 application upload\"}");
  const esp_partition_t *partition=esp_ota_get_next_update_partition(nullptr);
  if(!partition||req->content_len<288||req->content_len>(int)partition->size)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_image\",\"message\":\"Invalid image size\"}");
  otaActive=true;uint8_t buffer[1024];int left=req->content_len;size_t used=0;
  // Validate embedded IDF application descriptor before erasing inactive slot.
  const size_t prefix=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t)+sizeof(daikin_target);
  while(used<prefix){int n=httpd_req_recv(req,(char*)buffer+used,prefix-used);if(n<=0){otaActive=false;return ESP_FAIL;}used+=n;}
  auto *desc=(esp_app_desc_t*)(buffer+sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t));
  const esp_app_desc_t *self=esp_ota_get_app_description();
  bool correct=buffer[0]==0xe9&&desc->magic_word==ESP_APP_DESC_MAGIC_WORD&&memcmp(desc->project_name,self->project_name,sizeof(desc->project_name))==0&&memcmp(buffer+prefix-sizeof(daikin_target),daikin_target,sizeof(daikin_target))==0;
  // Custom descriptor prevents other ESP32 products or PM variants being installed.
  esp_ota_handle_t handle=0;esp_err_t error=correct?esp_ota_begin(partition,req->content_len,&handle):ESP_ERR_INVALID_ARG;
  if(error==ESP_OK){error=esp_ota_write(handle,buffer,used);left-=used;}
  while(error==ESP_OK&&left>0){int n=httpd_req_recv(req,(char*)buffer,min(left,(int)sizeof(buffer)));if(n<=0){error=ESP_FAIL;break;}error=esp_ota_write(handle,buffer,n);left-=n;}
  if(error==ESP_OK){error=esp_ota_end(handle);handle=0;}if(handle)esp_ota_abort(handle);
  if(error==ESP_OK)error=esp_ota_set_boot_partition(partition);
  otaActive=false;
  if(error!=ESP_OK)return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_image\",\"message\":\"Image validation or flash write failed\"}");
  rebootAt=millis()+1500;
  return jsonResponse(req,"202 Accepted","{\"code\":\"accepted\",\"message\":\"Image verified; rebooting\"}");
}
