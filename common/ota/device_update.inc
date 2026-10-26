/* Shared job reservation serializes metadata, release notes and installation. */
#include "fetch.h"
#include "version.h"
#include "../firmware_variant.h"
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
#define UPDATE_PRIORITY (MICO_APPLICATION_PRIORITY+1)
#elif defined(FEATURE_DEVICE_A1)
#define UPDATE_MODEL "a1"
#else
#define UPDATE_MODEL "m1"
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
static const char *update_text(json_object *o,const char *key){json_object *v=json_object_object_get(o,key);return v&&json_object_is_type(v,json_type_string)?json_object_get_string(v):"";}
static bool update_relative(const char *s){
    if(strncmp(s,"/releases/v",11)||strlen(s)>=200||strstr(s,".."))return false;
    for(;*s;s++)if(!isalnum((unsigned char)*s)&&!strchr("/._-",*s))return false;return true;
}
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
            snprintf(url,sizeof(url),"%s/ota/v1/" UPDATE_MODEL "/" PHICOMM_FIRMWARE_VARIANT "/latest.json",update_bases[update_source]);
            ok=ota_fetch(url,4096,meta_begin,meta_write,&b,&error);
            /* TLS buffers are already free before JSON parsing. */
            if(ok){
                b.body[b.length]=0;json_object *meta=service_parse(b.body,b.length);ok=false;error="Invalid release manifest";
                if(meta){
                    const char *v=update_text(meta,"version"),*path=update_text(meta,"path"),*notes=update_text(meta,"release_log");ph_ota_version_t remote,local;
                    size_t n=strlen(path);
                    if(!strcmp(update_text(meta,"model"),UPDATE_MODEL)&&
                       (!strcmp(update_text(meta,"firmware_variant"),PHICOMM_FIRMWARE_VARIANT)||(PHICOMM_LEGACY_FULL_VARIANT&&!update_text(meta,"firmware_variant")[0]))&&strlen(v)<sizeof(update_version)&&ph_ota_version_parse(v,&remote)&&ph_ota_version_parse(FIRMWARE_VERSION,&local)&&update_relative(path)&&n>7&&!strcmp(path+n-7,".signed")&&update_relative(notes)){
                        mico_rtos_lock_mutex(&service_lock);strcpy(update_version,v);strcpy(update_path,path);strcpy(update_notes,notes);update_available=false;
                        update_available=ph_ota_version_compare(&remote,&local)>0;
                        mico_rtos_unlock_mutex(&service_lock);ok=true;
                    }
                    json_object_put(meta);
                }
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
static void update_capabilities(json_object *o){json_object *a=json_object_object_get(o,"actions");if(a)for(unsigned i=0;update_capability_names[i];i++)json_build_array_add(a,json_object_new_string(update_capability_names[i]));}
static json_object *update_command(const char *action,json_object *p,const char **code,bool *handled){
    if(strcmp(action,"ota.sources.get")&&strcmp(action,"ota.check")&&strcmp(action,"ota.check.get")&&strcmp(action,"ota.install")&&strcmp(action,"ota.notes")&&strcmp(action,"ota.notes.get"))return NULL;
    *handled=true;*code="ok";json_object *o=json_object_new_object();
    if(!strcmp(action,"ota.sources.get")){json_object *a=json_object_new_array();for(unsigned i=0;i<3;i++)if(*update_bases[i])json_build_array_add(a,json_object_new_string(update_sources[i]));json_build_object_add(o,"sources",a);return o;}
    mico_rtos_lock_mutex(&service_lock);
    if(!strcmp(action,"ota.check")||!strcmp(action,"ota.notes")){
        bool notes=!strcmp(action,"ota.notes");const char *source=update_text(p,"source");int selected=notes?(int)update_source:!strcmp(source,"github")?0:!strcmp(source,"cn")?1:!strcmp(source,"global")?2:-1;
        json_object *offset=json_object_object_get(p,"offset");int at=offset?json_object_get_int(offset):0;
        if(selected<0||!*update_bases[selected]||(notes&&(!*update_notes||at<0||at>16384||(offset&&!json_object_is_type(offset,json_type_int)))))*code="invalid_params";
        else if(update_busy||!ota_job_claim())*code="busy";
        else{
            update_source=selected;update_busy=true;update_notes_job=notes;update_error[0]=0;
            if(notes){notes_offset=at;notes_used=notes_total=0;notes_text[0]=0;strcpy(notes_state,"loading");}
            else{update_available=false;update_version[0]=update_path[0]=update_notes[0]=0;strcpy(update_state,"checking");}
            if(mico_rtos_create_thread(NULL,UPDATE_PRIORITY,"ota_metadata",update_worker,6144,0)!=kNoErr){update_busy=false;strcpy(notes?notes_state:update_state,"failed");ota_job_release();*code="busy";}else *code="accepted";
        }
    }else if(!strcmp(action,"ota.install")){
        if(update_busy||!update_available||!*update_path)*code="invalid_params";
        else{char url[500];snprintf(url,sizeof(url),"%s%s",update_bases[update_source],update_path);json_object *params=json_object_new_object();string(params,"url",url);mico_rtos_unlock_mutex(&service_lock);json_object_put(o);o=service_product("ota.start",params,code);json_object_put(params);return o;}
    }
    if(!strcmp(action,"ota.notes.get")){
        string(o,"state",notes_state);string(o,"text",!strcmp(notes_state,"ready")?notes_text:"");
        json_build_object_add(o,"offset",json_object_new_int(notes_offset));json_build_object_add(o,"next",json_object_new_int(notes_offset+notes_used));json_build_object_add(o,"total",json_object_new_int(notes_total));
    }else{
        string(o,"state",update_state);string(o,"version",update_version);string(o,"source",update_sources[update_source]);
        if(*update_notes){char url[500];snprintf(url,sizeof(url),"%s%s",update_bases[update_source],update_notes);string(o,"release_log",url);}
        json_build_object_add(o,"available",json_object_new_boolean(update_available));
    }
    string(o,"error",update_error);json_build_object_add(o,"busy",json_object_new_boolean(update_busy));
    mico_rtos_unlock_mutex(&service_lock);return o;
}
