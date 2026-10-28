#include "mico.h"
#include "mode.h"
#include "../service_mp.h"
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
#ifndef OTA_MODEL_TC1
static bool restoring, recovery_busy;
static uint32_t recovery_at, generation;
#endif
static uint32_t heap_before,heap_quiesced,heap_min,heap_restored;
static const char *phase="idle";
void ota_job_init(void){mico_rtos_init_mutex(&job_lock);}
bool ota_job_active(void){mico_rtos_lock_mutex(&job_lock);bool yes=claimed;mico_rtos_unlock_mutex(&job_lock);return yes;}
bool ota_job_claim(void){mico_rtos_lock_mutex(&job_lock);bool ok=!claimed;if(ok){claimed=true;cancelled=committing=false;
#ifndef OTA_MODEL_TC1
++generation;restoring=false;
#endif
heap_before=heap_min=MicoGetMemoryInfo()->free_memory;heap_quiesced=heap_restored=0;phase="reserved";}mico_rtos_unlock_mutex(&job_lock);return ok;}
void ota_job_sample(void){uint32_t n=MicoGetMemoryInfo()->free_memory;mico_rtos_lock_mutex(&job_lock);if(claimed&&n<heap_min)heap_min=n;mico_rtos_unlock_mutex(&job_lock);}
bool ota_job_cancel(void){mico_rtos_lock_mutex(&job_lock);bool ok=claimed&&!committing;if(ok)cancelled=true;mico_rtos_unlock_mutex(&job_lock);return ok;}
bool ota_job_cancelled(void){mico_rtos_lock_mutex(&job_lock);bool yes=cancelled;mico_rtos_unlock_mutex(&job_lock);return yes;}
bool ota_job_commit(void){mico_rtos_lock_mutex(&job_lock);bool ok=!cancelled;if(ok){committing=true;phase="committing";}mico_rtos_unlock_mutex(&job_lock);return ok;}
static void job_phase(const char *s){mico_rtos_lock_mutex(&job_lock);phase=s;mico_rtos_unlock_mutex(&job_lock);}
bool ota_job_enter(void){
    job_phase("quiescing");
#ifndef OTA_MODEL_TC1
    mico_rtos_lock_mutex(&job_lock);stopped=true;mico_rtos_unlock_mutex(&job_lock);
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
void ota_job_poll(void){
#ifndef OTA_MODEL_TC1
    uint32_t at=mico_rtos_get_time(),token;
    mico_rtos_lock_mutex(&job_lock);
    if(!restoring || recovery_busy || (int32_t)(at-recovery_at)<0){mico_rtos_unlock_mutex(&job_lock);return;}
    recovery_busy=true;token=generation;recovery_at=at+1000u;
    mico_rtos_unlock_mutex(&job_lock);
    /* No application lock is held across runtime/thread creation. The step
     * never waits for allocation recovery, worker cleanup or readiness. */
    bool ready=feature_resume_step()==kNoErr;
    mico_rtos_lock_mutex(&job_lock);
    if(token==generation && restoring && ready){
        stopped=restoring=false;heap_restored=MicoGetMemoryInfo()->free_memory;claimed=false;phase="idle";
    }
    recovery_busy=false;mico_rtos_unlock_mutex(&job_lock);
#endif
}
void ota_job_release(void){
#ifdef OTA_MODEL_TC1
    job_phase("restoring");
#endif
    mico_rtos_lock_mutex(&job_lock);
#ifndef OTA_MODEL_TC1
    if(stopped){
        /* Keep OTA exclusion until the old worker is actually ready again.
         * Duplicate releases neither clear a claim nor postpone retries. */
        if(!restoring){restoring=true;recovery_at=mico_rtos_get_time();phase="restoring";}
        mico_rtos_unlock_mutex(&job_lock);return;
    }
#endif
    heap_restored=MicoGetMemoryInfo()->free_memory;claimed=false;phase="idle";
    mico_rtos_unlock_mutex(&job_lock);
}
void ota_job_status_mp(ph_writer *o){
    phw_begin(o,NULL,false);char state[32],error[128];unsigned progress;
#ifdef OTA_MODEL_A1
    mico_rtos_lock_mutex(&a1_lock);snprintf(state,sizeof(state),"%s",a1_state.ota_state);snprintf(error,sizeof(error),"%s",a1_state.ota_error);progress=a1_state.ota_progress;mico_rtos_unlock_mutex(&a1_lock);
#elif defined(OTA_MODEL_M1)
    mico_rtos_lock_mutex(&m1_lock);snprintf(state,sizeof(state),"%s",m1_state.ota_state);snprintf(error,sizeof(error),"%s",m1_state.ota_error);progress=m1_state.ota_progress;mico_rtos_unlock_mutex(&m1_lock);
#else
    tc1_ota_snapshot(state,error,&progress);
#endif
    phw_text(o,"device_id",JOB_ID);
    phw_text(o,"version",FIRMWARE_VERSION);
    phw_text(o,"ota_state",state);phw_text(o,"ota_error",error);
    phw_int(o,"ota_progress",progress);
    mico_rtos_lock_mutex(&job_lock);phw_text(o,"ota_mode",phase);phw_bool(o,"ota_active",claimed);phw_int(o,"heap_before",heap_before);phw_int(o,"heap_quiesced",heap_quiesced);phw_int(o,"heap_min",heap_min);phw_int(o,"heap_restored",heap_restored);mico_rtos_unlock_mutex(&job_lock);
    phw_int(o,"free_memory",MicoGetMemoryInfo()->free_memory);
#ifndef OTA_MODEL_TC1
    phw_bool(o,"binding_running",feature_running());
#endif
    phw_end(o);
}
