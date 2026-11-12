/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included by ph_port.c; command admission and state use bl_lock. */
#ifdef PH_HOST_TEST
#include "httpclient_host.h"
#include "../../common/ota/release_manifest.h"
#else
#include "../../httpclient/http_client.h"
#include "../common/ota/release_manifest.h"
#include "update_config.h"
#endif
#ifndef PHICOMM_OTA_METADATA_PREFIX
#define PHICOMM_OTA_METADATA_PREFIX "/ota/v1"
#endif
#ifndef PHICOMM_OTA_CN_BASE_URL
#define PHICOMM_OTA_CN_BASE_URL "http://phicomm.fjrcn.cn"
#endif
#ifndef PHICOMM_OTA_GLOBAL_BASE_URL
#define PHICOMM_OTA_GLOBAL_BASE_URL "http://phicomm.fjrcn.com"
#endif
static const char *const update_bases[]={PHICOMM_OTA_CN_BASE_URL,PHICOMM_OTA_GLOBAL_BASE_URL};
static const char *const update_sources[]={"cn","global"};
static bool update_pending,update_available;
static unsigned update_source;
static uint32_t update_at;
static const char *update_state="idle",*update_error="";
static ph_release_manifest update_manifest;
typedef struct {
 httprequest_t request;
 char url[128],buffer[513];
 unsigned used;
 bool failed;
 unsigned char body[4096];
} update_transfer;

static const char *ota_start(const char *url,const char *sha256,uint32_t size) {
 if(strncmp(url,"http://",7)||strlen(url)>=sizeof(ota_url)||strpbrk(url,"\r\n\t ")||strlen(url)<12)return "invalid_params";
 if(bl_ota_pending||OTA_GetProgress()>=0||reboot_pending||reset_requested||scan_state)return "busy";
 /* The native downloader reboots outside the normal product reboot path. */
 if(!checkpoint(now(NULL),true))return "storage_error";
 if(!PH_BL_OTAHashSet(sha256,size))return "invalid_params";
 strcpy(ota_url,url);ota_at=now(NULL)+1500;bl_ota_pending=true;
 ph_device_set_maintenance(&bl_device,true);ota_failed=false;return "accepted";
}
static void update_finish(const char *error) {
 update_error=error;update_state=*error?"failed":update_available?"available":"up_to_date";
 update_pending=update_busy=bl_ota_pending=false;
 ph_device_set_maintenance(&bl_device,false);
}
static int update_callback(httprequest_t *request) {
 update_transfer *transfer=(update_transfer *)request;
 httpclient_data_t *data=&request->client_data;
 if(request->state<0)transfer->failed=true;
 if(request->state==1) {
  if(request->client.response_code!=200||data->is_chunked||!data->response_content_len||
     data->response_content_len>sizeof(transfer->body)||data->response_buf_filled>sizeof(transfer->body)-transfer->used) {
   transfer->failed=true;return 1;
  }
  memcpy(transfer->body+transfer->used,data->response_buf,data->response_buf_filled);
  transfer->used+=data->response_buf_filled;
 }
 if(request->state==2) {
  ph_release_manifest manifest;ph_ota_version_t remote,local;
  const char *error="Download failed";
  lock();
  if(!transfer->failed&&transfer->used&&transfer->used==data->response_content_len) {
   error="Invalid release manifest";
   if(ph_release_manifest_parse(transfer->body,transfer->used,"s30","full",&manifest)&&
      manifest.size<=0xAE000u&&ph_ota_version_parse(manifest.version,&remote)&&ph_ota_version_parse(bl_device.version,&local)) {
    char path[200];
    snprintf(path,sizeof(path),"/releases/v%s/BL2028N_OTA_%s.rbl",manifest.version,manifest.version);
    if(!strcmp(path,manifest.path)&&strlen(update_bases[update_source])+strlen(path)<sizeof(ota_url)) {
     update_manifest=manifest;update_available=ph_ota_version_compare(&remote,&local)>0;error="";
    }
   }
  }
  update_finish(error);unlock();
 }
 return 0;
}
static void update_step(uint32_t t) {
 if(!update_pending)return;
 if(!bl_mqtt_quiescent||xPortGetFreeHeapSize()<PH_SERVICE_MEMORY_RESERVE+sizeof(update_transfer)+8192u) {
  if((uint32_t)(t-update_at)>=10000)update_finish("Not enough free memory");
  return;
 }
 update_pending=false;
 update_transfer *transfer=os_malloc(sizeof(*transfer));
 if(!transfer){update_finish("Not enough free memory");return;}
 memset(transfer,0,sizeof(*transfer));
 int n=snprintf(transfer->url,sizeof(transfer->url),"%s" PHICOMM_OTA_METADATA_PREFIX "/s30/full/latest.msgpack",update_bases[update_source]);
 if(n<0||(size_t)n>=sizeof(transfer->url)){os_free(transfer);update_finish("Update URL too long");return;}
 httprequest_t *request=&transfer->request;
 request->url=transfer->url;request->port=80;request->method=HTTPCLIENT_GET;request->timeout=15000;
 request->header="Accept: application/msgpack\r\nAccept-Encoding: identity\r\n";
 request->flags=HTTPREQUEST_FLAG_FREE_SELFONDONE;request->data_callback=update_callback;
 request->client_data.response_buf=transfer->buffer;request->client_data.response_buf_len=sizeof(transfer->buffer);
 if(HTTPClient_Async_SendGeneric(request)!=0){os_free(transfer);update_finish("Cannot start download");}
}
static const char *update_action(const char *action,const ph_value *p,ph_writer *w) {
 if(!strcmp(action,"ota.sources.get")) {
  if(p->count)return "invalid_params";
  phw_begin(w,"sources",true);for(unsigned i=0;i<2;i++)phw_text(w,NULL,update_sources[i]);phw_end(w);return "ok";
 }
 if(!strcmp(action,"ota.check.get")) {
  if(p->count)return "invalid_params";
 }else if(!strcmp(action,"ota.check")) {
  char source[8];
  if(p->count!=1||!get_text(p,"source",source,sizeof(source)))return "invalid_params";
  unsigned selected=!strcmp(source,"cn")?0:!strcmp(source,"global")?1:2;
  if(selected==2)return "invalid_params";
  if(bl_ota_pending||OTA_GetProgress()>=0||reboot_pending||reset_requested||scan_state)return "busy";
  update_source=selected;update_available=false;memset(&update_manifest,0,sizeof(update_manifest));
  update_state="checking";update_error="";update_at=now(NULL);
  update_pending=update_busy=bl_ota_pending=true;ph_device_set_maintenance(&bl_device,true);
 }else if(!strcmp(action,"ota.install")) {
  if(p->count||!update_available||strcmp(update_state,"available"))return "invalid_params";
  char url[128];snprintf(url,sizeof(url),"%s%s",update_bases[update_source],update_manifest.path);
  return ota_start(url,update_manifest.sha256,update_manifest.size);
 }else return "unsupported";
 phw_text(w,"state",update_state);phw_text(w,"version",update_manifest.version);
 phw_text(w,"source",update_sources[update_source]);phw_text(w,"error",update_error);
 phw_bool(w,"available",update_available);phw_bool(w,"busy",update_busy);
 return !strcmp(action,"ota.check")?"accepted":"ok";
}
