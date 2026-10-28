/* Test SDK/job/fetch boundaries only. The actual complete service.c, metadata
 * worker, version comparison and manifest reader are compiled unchanged. */
#include "common/service_mp.h"
#include "common/ota/fetch.h"
#include "mico.h"
#include <stdio.h>
#include <string.h>
const char *test_local_version="1.0.0-rc.4";
static bool claimed;
static void (*pending_worker)(mico_thread_arg_t);
static mico_thread_arg_t pending_arg;
ph_buf *test_manifest;
bool test_fetch_ok=true;
unsigned test_fetch_calls;
char test_fetch_url[512];
void ota_job_init(void){}
bool ota_job_claim(void){if(claimed)return false;claimed=true;return true;}
bool ota_job_enter(void){return claimed;}
void ota_job_release(void){claimed=false;}
bool ota_job_active(void){return claimed;}
bool ota_job_cancel(void){bool old=claimed;claimed=false;return old;}
void ota_job_status_mp(ph_writer *w){phw_begin(w,NULL,false);phw_bool(w,"active",claimed);phw_end(w);}
int mico_rtos_create_thread(void *id,int priority,const char *name,void (*worker)(mico_thread_arg_t),unsigned stack,mico_thread_arg_t arg){
 (void)id;(void)priority;assert(!strcmp(name,"ota_metadata")&&stack==6144&&!pending_worker);
 pending_worker=worker;pending_arg=arg;return kNoErr;
}
void mico_rtos_delete_thread(void *id){assert(!id);}
void test_run_worker(void){assert(pending_worker);void (*worker)(mico_thread_arg_t)=pending_worker;pending_worker=NULL;worker(pending_arg);}
bool ota_fetch(const char *url,uint32_t limit,ota_fetch_begin begin,ota_fetch_write write,void *ctx,const char **error){
 test_fetch_calls++;assert(strlen(url)<sizeof(test_fetch_url));strcpy(test_fetch_url,url);assert(limit==4096);
 if(!test_fetch_ok){*error="injected transfer failure";return false;}
 assert(test_manifest&&test_manifest->len<=limit);
 if(!begin(ctx,(uint32_t)test_manifest->len)){*error="injected begin failure";return false;}
 return write(ctx,0,(const uint8_t *)test_manifest->buf,test_manifest->len);
}
