#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "common/service.h"
static unsigned mutations,captures,caps,mqtt_calls;
static bool change_during_capture,fail_capture,caps_restricted;
static bool exhaust_capture,exhaust_mutation;
static unsigned large_calls;
extern uint32_t test_heap_free;
static long allocation_budget=-1;
static char async_session[64];
extern uint32_t test_clock_ms;
void *__real_malloc(size_t);
void __real_free(void *);
/* The wrapper owns every direct linked malloc, including retained cache and
 * receipt buffers. A naturally aligned header lets unknown/double frees fail
 * immediately and counts exact live ownership; crypto's shared internals are
 * outside --wrap and deliberately outside these accounting assertions. */
typedef union { struct {size_t bytes;uint64_t magic;} owned;long double align;void *pointer; } allocation_header;
static pthread_mutex_t allocation_lock=PTHREAD_MUTEX_INITIALIZER;
static size_t live_blocks,live_bytes,total_allocations,total_frees;
#define ALLOCATION_MAGIC UINT64_C(0x9d0a473e65bc128f)
void *__wrap_malloc(size_t n){
 pthread_mutex_lock(&allocation_lock);
 if(!allocation_budget){pthread_mutex_unlock(&allocation_lock);return NULL;}
 if(allocation_budget>0)--allocation_budget;
 allocation_header *h=__real_malloc(sizeof(*h)+n);
 if(h){h->owned.bytes=n;h->owned.magic=ALLOCATION_MAGIC;live_blocks++;live_bytes+=n;total_allocations++;}
 pthread_mutex_unlock(&allocation_lock);return h?h+1:NULL;
}
void __wrap_free(void *p){
 if(!p)return;allocation_header *h=(allocation_header *)p-1;
 pthread_mutex_lock(&allocation_lock);assert(h->owned.magic==ALLOCATION_MAGIC);
 assert(live_blocks&&live_bytes>=h->owned.bytes);live_blocks--;live_bytes-=h->owned.bytes;total_frees++;
 h->owned.magic=0;pthread_mutex_unlock(&allocation_lock);__real_free(h);
}
extern bool device_config_recovery(void);
extern void device_config_recovery_set(bool);
static ph_buf *request(const char *action,const char *id,int full){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action",action);
 if(id)phw_text(&w,"request_id",id);phw_begin(&w,"params",false);
 if(full>=0)phw_bool(&w,"full",full);phw_end(&w);phw_end(&w);return phw_take(&w);
}
static ph_value data_value(ph_buf *out){ph_value root,data;assert(out&&phv_parse(1,out->buf,out->len,&root)&&phv_get(&root,"data",&data));return data;}
static void code(ph_buf *out,const char *expected){ph_value root,v;assert(out&&phv_parse(1,out->buf,out->len,&root)&&phv_get(&root,"code",&v)&&phv_equal_text(&v,expected));}
static void command_code(ph_buf *request,const char *expected){ph_buf *out=service_command_mp(request->buf,request->len);code(out,expected);ph_buf_free(out);ph_buf_free(request);}
static void object(ph_writer *w){phw_begin(w,NULL,false);phw_u32(w,"value",123);phw_end(w);}
void service_product_status_mp(ph_writer *w){captures++;if(change_during_capture){change_during_capture=false;service_event("info","system","boot");}if(fail_capture){fail_capture=false;w->failed=true;return;}if(exhaust_capture){exhaust_capture=false;test_heap_free=SERVICE_MEMORY_RESERVE;char large[1200];memset(large,'x',sizeof(large));phw_begin(w,NULL,false);phw_text_n(w,"large",large,sizeof(large));phw_end(w);return;}object(w);}
void service_product_capabilities_mp(ph_writer *w,const char *const *extra,bool restricted){(void)extra;caps_restricted=restricted;caps++;object(w);}
void service_product_info_mp(ph_writer *w){object(w);}
bool service_product_mp(const char *action,const ph_value *p,ph_writer *w,const char **out){
 if(!strcmp(action,"large.set")){
  ph_value v;const void *bytes;size_t length;
  if(!phv_keys(p,"|payload|")||!phv_get(p,"payload",&v)||!phv_bytes(&v,&bytes,&length)||length!=7000){*out="invalid_params";object(w);return true;}
  assert(((const unsigned char *)bytes)[0]==0x41);large_calls++;mutations++;*out="accepted";object(w);return true;
 }
 if(strcmp(action,"system.reset")&&strcmp(action,"fan.set")&&strcmp(action,"system.reboot")&&strcmp(action,"wifi.scan"))return false;
 if(!phv_keys(p,!strcmp(action,"system.reset")?"|full|":"")){*out="invalid_params";phw_begin(w,NULL,false);phw_end(w);return true;}
 mutations++;*out="accepted";
 if(exhaust_mutation){exhaust_mutation=false;phw_begin(w,NULL,false);phw_int(w,"first",1);test_heap_free=SERVICE_MEMORY_RESERVE;char large[1200];memset(large,'x',sizeof(large));phw_text_n(w,"large",large,sizeof(large));phw_end(w);return true;}
 object(w);return true;
}
bool service_product_mqtt_mp(const char *action,const ph_value *p,ph_writer *w,const char **out){
 if(strcmp(action,"sensor.session.set"))return false;mqtt_calls++;ph_value v;
 if(!phv_keys(p,"|session|")||!phv_get(p,"session",&v)||!phv_text(&v,async_session,sizeof(async_session))){*out="invalid_params";phw_begin(w,NULL,false);phw_end(w);return true;}
 *out="accepted";object(w);return true;
}
bool automation_mp(const char *a,const ph_value *p,ph_writer *w,const char **out){(void)a;(void)p;(void)w;(void)out;return false;}
#ifdef TEST_ACTUAL_OTA_WORKER
static unsigned installs;
static char installed_url[512];
bool service_product_ota_start(const char *url){assert(strlen(url)<sizeof(installed_url));strcpy(installed_url,url);installs++;return true;}
#else
bool service_product_ota_start(const char *url){(void)url;return true;}
#endif

#ifdef TEST_ILP32_STRTOL
#include <errno.h>
#include <limits.h>
extern long __real_strtol(const char *,char **,int);
/* Host long is 64 bits; the target libc has a 32-bit saturating strtol.
 * Preserve stale errno exactly as libc does on otherwise successful parses. */
long __wrap_strtol(const char *s,char **end,int base){
 long n=__real_strtol(s,end,base);
 if(n>INT32_MAX){errno=ERANGE;return INT32_MAX;}
 if(n<INT32_MIN){errno=ERANGE;return INT32_MIN;}
 return n;
}
#endif
