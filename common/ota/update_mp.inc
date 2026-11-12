/* Native MP metadata/control. Asynchronous workers own their parameters. */
#ifdef PHICOMM_DEVICE_OTA
/* Shared job reservation serializes metadata, release notes and installation. */
#include "fetch.h"
#include "version.h"
#include "../firmware_variant_defs.h"
#include "release_manifest.h"
#ifndef PHICOMM_OTA_GITHUB_BASE_URL
#define PHICOMM_OTA_GITHUB_BASE_URL "https://phicomm-io.github.io"
#endif
#ifndef PHICOMM_OTA_CN_BASE_URL
#define PHICOMM_OTA_CN_BASE_URL "http://phicomm.fjrcn.cn"
#endif
#ifndef PHICOMM_OTA_GLOBAL_BASE_URL
#define PHICOMM_OTA_GLOBAL_BASE_URL "http://phicomm.fjrcn.com"
#endif
#if defined(AUTOMATION_TC1)
#define UPDATE_MODEL "tc1"
#define UPDATE_MANIFEST_MODEL "TC1"
#define UPDATE_PRIORITY (MICO_APPLICATION_PRIORITY+1)
#elif defined(FEATURE_DEVICE_A1)
#define UPDATE_MODEL "a1"
#define UPDATE_MANIFEST_MODEL "A1"
#else
#define UPDATE_MODEL "m1"
#define UPDATE_MANIFEST_MODEL "M1"
#endif
#ifndef UPDATE_PRIORITY
#define UPDATE_PRIORITY MICO_APPLICATION_PRIORITY
#endif
static const char *update_bases[]={PHICOMM_OTA_GITHUB_BASE_URL,PHICOMM_OTA_CN_BASE_URL,PHICOMM_OTA_GLOBAL_BASE_URL};
static const char *update_sources[]={"github","cn","global"};
static bool update_busy,update_available,update_notes_job;
static unsigned update_source;
static char update_state[24]="idle",update_version[32],update_path[200],update_notes[200];
static char update_error[80],notes_state[16]="idle",notes_text[1025];
static uint32_t notes_offset,notes_total,notes_used;
typedef struct{char *body;uint32_t length;} metadata_body;
static bool meta_begin(void *ctx,uint32_t n){metadata_body *b=ctx;b->body=malloc(n+1);b->length=n;return b->body!=NULL;}
static bool meta_write(void *ctx,uint32_t at,const uint8_t *p,size_t n){metadata_body *b=ctx;if(at+n>b->length)return false;memcpy(b->body+at,p,n);return true;}
static bool notes_begin(void *ctx,uint32_t n){(void)ctx;mico_rtos_lock_mutex(&service_lock);notes_total=n;notes_used=0;bool ok=notes_offset<=n;mico_rtos_unlock_mutex(&service_lock);return ok;}
static bool notes_write(void *ctx,uint32_t at,const uint8_t *p,size_t n){
    (void)ctx;
    mico_rtos_lock_mutex(&service_lock);
    if(at+n<=notes_offset||notes_used>=1024){mico_rtos_unlock_mutex(&service_lock);return true;}
    if(at<notes_offset){unsigned skip=notes_offset-at;p+=skip;n-=skip;}
    if(n>1024-notes_used)n=1024-notes_used;
    if(!notes_used && n && (p[0]&0xc0)==0x80){mico_rtos_unlock_mutex(&service_lock);return false;}
    memcpy(notes_text+notes_used,p,n);notes_used+=n;mico_rtos_unlock_mutex(&service_lock);return true;
}
static void trim_utf8(void){
    /* Do not split a UTF-8 codepoint at the response boundary. */
    if(notes_used && notes_offset+notes_used<notes_total){
        unsigned start=notes_used-1;while(start && ((unsigned char)notes_text[start]&0xc0)==0x80)start--;
        unsigned char ch=notes_text[start];unsigned need=ch<0x80?1:ch<0xe0?2:ch<0xf0?3:4;
        if(notes_used-start<need)notes_used=start;
    }
    notes_text[notes_used]=0;
}
static void update_worker(mico_thread_arg_t arg){
    (void)arg;const char *error="Cannot stop linkage worker";bool ok=false;metadata_body b={0};char url[500];
    if(ota_job_enter()){
        if(update_notes_job){
            snprintf(url,sizeof(url),"%s%s",update_bases[update_source],update_notes);
            ok=ota_fetch(url,16384,notes_begin,notes_write,NULL,&error);
            if(ok){mico_rtos_lock_mutex(&service_lock);trim_utf8();mico_rtos_unlock_mutex(&service_lock);}
        }else{
            snprintf(url,sizeof(url),"%s" PHICOMM_OTA_METADATA_PREFIX "/" UPDATE_MODEL "/" PHICOMM_FIRMWARE_VARIANT "/latest.msgpack",update_bases[update_source]);
            ok=ota_fetch(url,4096,meta_begin,meta_write,&b,&error);
            /* TLS buffers are released before bounded borrowed-view parsing. */
            if(ok){
                ph_release_manifest manifest;ph_ota_version_t remote,local;
                ok=ph_release_manifest_parse(b.body,b.length,UPDATE_MANIFEST_MODEL,PHICOMM_FIRMWARE_VARIANT,&manifest)&&
                   ph_ota_version_parse(manifest.version,&remote)&&ph_ota_version_parse(FIRMWARE_VERSION,&local);
                if(ok){
                    mico_rtos_lock_mutex(&service_lock);
                    strcpy(update_version,manifest.version);strcpy(update_path,manifest.path);strcpy(update_notes,manifest.release_log);
                    update_available=ph_ota_version_compare(&remote,&local)>0;
                    mico_rtos_unlock_mutex(&service_lock);
                }else error="Invalid release manifest";
            }
        }
    }
    free(b.body);
    mico_rtos_lock_mutex(&service_lock);
    if(update_notes_job)strcpy(notes_state,ok?"ready":"failed");else strcpy(update_state,ok?(update_available?"available":"up_to_date"):"failed");
    snprintf(update_error,sizeof(update_error),"%s",ok?"":error?error:"Transfer failed");
    mico_rtos_unlock_mutex(&service_lock);
    ota_job_release();
    mico_rtos_lock_mutex(&service_lock);update_busy=false;mico_rtos_unlock_mutex(&service_lock);
    mico_rtos_delete_thread(NULL);
}
static const char *const update_capability_names[]={"ota.sources.get","ota.check","ota.check.get","ota.install","ota.notes","ota.notes.get","ota.status.get","ota.cancel",NULL};

static bool update_command_mp(const char *action,const ph_value *p,ph_writer *w,const char **code)
{
 if(strcmp(action,"ota.sources.get")&&strcmp(action,"ota.check")&&strcmp(action,"ota.check.get")&&strcmp(action,"ota.install")&&strcmp(action,"ota.notes")&&strcmp(action,"ota.notes.get"))return false;
 *code="ok";
 if(!strcmp(action,"ota.sources.get")){
  if(!phv_keys(p,"")){*code="invalid_params";phw_begin(w,NULL,false);phw_end(w);return true;}
  phw_begin(w,NULL,false);phw_begin(w,"sources",true);for(unsigned i=0;i<3;i++)if(*update_bases[i])phw_text(w,NULL,update_sources[i]);phw_end(w);phw_end(w);return true;
 }
 bool checking=!strcmp(action,"ota.check"),notes=!strcmp(action,"ota.notes");
 ph_value field;char source[8]={0};int64_t offset=0;
 bool valid=phv_keys(p,checking?"|source|":notes?"|offset|":"");
 if(checking)valid=valid&&phv_get(p,"source",&field)&&phv_text(&field,source,sizeof(source));
 if(notes&&phv_get(p,"offset",&field))valid=valid&&phv_int(&field,&offset)&&offset>=0&&offset<=16384;
 if(!valid){*code="invalid_params";phw_begin(w,NULL,false);phw_end(w);return true;}
 mico_rtos_lock_mutex(&service_lock);
 if(checking||notes){
  int selected=notes?(int)update_source:!strcmp(source,"github")?0:!strcmp(source,"cn")?1:!strcmp(source,"global")?2:-1;
  if(selected<0||!*update_bases[selected]||(notes&&!*update_notes))*code="invalid_params";
  else if(update_busy||!ota_job_claim())*code="busy";
  else{
   update_source=selected;update_busy=true;update_notes_job=notes;update_error[0]=0;
   if(notes){notes_offset=(uint32_t)offset;notes_used=notes_total=0;notes_text[0]=0;strcpy(notes_state,"loading");}
   else{update_available=false;update_version[0]=update_path[0]=update_notes[0]=0;strcpy(update_state,"checking");}
   if(mico_rtos_create_thread(NULL,UPDATE_PRIORITY,"ota_metadata",update_worker,6144,0)!=kNoErr){update_busy=false;strcpy(notes?notes_state:update_state,"failed");ota_job_release();*code="busy";}else *code="accepted";
  }
 }else if(!strcmp(action,"ota.install")){
  if(update_busy||!update_available||!*update_path)*code="invalid_params";
  else{
   char url[500];snprintf(url,sizeof(url),"%s%s",update_bases[update_source],update_path);
   mico_rtos_unlock_mutex(&service_lock);*code=service_product_ota_start(url)?"accepted":"busy";phw_begin(w,NULL,false);phw_end(w);return true;
  }
 }
 phw_begin(w,NULL,false);
 if(!strcmp(action,"ota.notes.get")){
  phw_text(w,"state",notes_state);phw_text(w,"text",!strcmp(notes_state,"ready")?notes_text:"");
  phw_u32(w,"offset",notes_offset);phw_u32(w,"next",notes_offset+notes_used);phw_u32(w,"total",notes_total);
 }else{
  phw_text(w,"state",update_state);phw_text(w,"version",update_version);phw_text(w,"source",update_sources[update_source]);
  if(*update_notes){char url[500];snprintf(url,sizeof(url),"%s%s",update_bases[update_source],update_notes);phw_text(w,"release_log",url);}
  phw_bool(w,"available",update_available);
 }
 phw_text(w,"error",update_error);phw_bool(w,"busy",update_busy);phw_end(w);
 mico_rtos_unlock_mutex(&service_lock);return true;
}

#else
static const char *const update_capability_names[]={NULL};
static bool update_command_mp(const char *a,const ph_value *p,ph_writer *w,const char **code){(void)a;(void)p;(void)w;(void)code;return false;}
#endif
