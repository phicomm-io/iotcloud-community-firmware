#include "mico.h"
#include "mode.h"
#include "../service.h"
#ifdef OTA_MODEL_A1
#include "a1.h"
#define JOB_ID a1_id
#define SNAPSHOT a1_ota_snapshot
#elif defined(OTA_MODEL_M1)
#include "m1.h"
#define JOB_ID m1_id
#else
#include "main.h"
#include "ota_server/signed_adapter.h"
#define JOB_ID str_mac
#endif
static mico_mutex_t job_lock;
static bool claimed, stopped, cancelled, committing;
static uint32_t heap_before,heap_quiesced,heap_min,heap_restored;
static const char *phase="idle";
void ota_job_init(void){mico_rtos_init_mutex(&job_lock);}
bool ota_job_active(void){mico_rtos_lock_mutex(&job_lock);bool yes=claimed;mico_rtos_unlock_mutex(&job_lock);return yes;}
bool ota_job_claim(void){mico_rtos_lock_mutex(&job_lock);bool ok=!claimed;if(ok){claimed=true;cancelled=committing=false;heap_before=heap_min=MicoGetMemoryInfo()->free_memory;heap_quiesced=heap_restored=0;phase="reserved";}mico_rtos_unlock_mutex(&job_lock);return ok;}
void ota_job_sample(void){uint32_t n=MicoGetMemoryInfo()->free_memory;mico_rtos_lock_mutex(&job_lock);if(claimed&&n<heap_min)heap_min=n;mico_rtos_unlock_mutex(&job_lock);}
bool ota_job_cancel(void){mico_rtos_lock_mutex(&job_lock);bool ok=claimed&&!committing;if(ok)cancelled=true;mico_rtos_unlock_mutex(&job_lock);return ok;}
bool ota_job_cancelled(void){mico_rtos_lock_mutex(&job_lock);bool yes=cancelled;mico_rtos_unlock_mutex(&job_lock);return yes;}
bool ota_job_commit(void){mico_rtos_lock_mutex(&job_lock);bool ok=!cancelled;if(ok){committing=true;phase="committing";}mico_rtos_unlock_mutex(&job_lock);return ok;}
static void job_phase(const char *s){mico_rtos_lock_mutex(&job_lock);phase=s;mico_rtos_unlock_mutex(&job_lock);}
bool ota_job_enter(void){
    job_phase("quiescing");
#ifndef OTA_MODEL_TC1
    stopped=true;
    if(!feature_pause(5000))return false;
#endif
#ifdef OTA_MODEL_TC1
    /* Let the HTTP owner apply admission control and drain old connections
       before reserving TLS buffers. Cancellation remains responsive. */
    for(unsigned i=0;i<25 && !ota_job_cancelled();i++)mico_thread_msleep(20);
#endif
    if(ota_job_cancelled())return false;
    mico_rtos_lock_mutex(&job_lock);heap_quiesced=MicoGetMemoryInfo()->free_memory;mico_rtos_unlock_mutex(&job_lock);
    job_phase("transfer");service_event("info","ota","transfer");return true;
}
void ota_job_release(void){
    job_phase("restoring");
#ifndef OTA_MODEL_TC1
    /* TLS must have been released first. Retry rather than silently abandon
       a stopped business worker when a transient allocation fails. */
    while(stopped && feature_resume()!=kNoErr)mico_thread_msleep(1000);
    stopped=false;
#endif
    mico_rtos_lock_mutex(&job_lock);heap_restored=MicoGetMemoryInfo()->free_memory;claimed=false;phase="idle";mico_rtos_unlock_mutex(&job_lock);
}
json_object *ota_job_status(void){
    json_object *o=json_object_new_object();char state[32],error[128];unsigned progress;
#ifdef OTA_MODEL_A1
    mico_rtos_lock_mutex(&a1_lock);snprintf(state,sizeof(state),"%s",a1_state.ota_state);snprintf(error,sizeof(error),"%s",a1_state.ota_error);progress=a1_state.ota_progress;mico_rtos_unlock_mutex(&a1_lock);
#elif defined(OTA_MODEL_M1)
    mico_rtos_lock_mutex(&m1_lock);snprintf(state,sizeof(state),"%s",m1_state.ota_state);snprintf(error,sizeof(error),"%s",m1_state.ota_error);progress=m1_state.ota_progress;mico_rtos_unlock_mutex(&m1_lock);
#else
    tc1_ota_snapshot(state,error,&progress);
#endif
    json_build_object_add(o,"device_id",json_object_new_string(JOB_ID));
    json_build_object_add(o,"version",json_object_new_string(FIRMWARE_VERSION));
    json_build_object_add(o,"ota_state",json_object_new_string(state));json_build_object_add(o,"ota_error",json_object_new_string(error));
    json_build_object_add(o,"ota_progress",json_object_new_int(progress));
    mico_rtos_lock_mutex(&job_lock);json_build_object_add(o,"ota_mode",json_object_new_string(phase));json_build_object_add(o,"ota_active",json_object_new_boolean(claimed));json_build_object_add(o,"heap_before",json_object_new_int(heap_before));json_build_object_add(o,"heap_quiesced",json_object_new_int(heap_quiesced));json_build_object_add(o,"heap_min",json_object_new_int(heap_min));json_build_object_add(o,"heap_restored",json_object_new_int(heap_restored));mico_rtos_unlock_mutex(&job_lock);
    json_build_object_add(o,"free_memory",json_object_new_int(MicoGetMemoryInfo()->free_memory));
#ifndef OTA_MODEL_TC1
    json_build_object_add(o,"binding_running",json_object_new_boolean(feature_running()));
#endif
    return o;
}
