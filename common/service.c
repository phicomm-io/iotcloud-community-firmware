/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "service_mp.h"
#include "backup.h"
#include "ota/mode.h"
#include "mico.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "generated/build_ota.h"
#include "firmware_variant_defs.h"
#include "sha.h"
__attribute__((weak)) bool device_config_recovery(void){return false;}
__attribute__((weak)) void device_config_recovery_set(bool recovery){(void)recovery;}
#define LOG_COUNT 64
/* Fixed vocabulary events never accept credentials, payloads or URLs. */
typedef struct {uint32_t sequence,uptime,utc;int32_t argument;uint8_t level,module,event,reserved;} service_record_t;
#include "log_dictionary.h"
static service_record_t records[LOG_COUNT];
static uint32_t sequence;
static mico_mutex_t service_lock,command_lock;
static bool ready,service_lock_ready;
/* Independent lock: writers may allocate while the service snapshot lock is
 * held. Failure accounting must never recursively take that lock. */
static mico_mutex_t wire_diag_lock;
static bool wire_diag_ready;
static uint32_t wire_alloc_failed,wire_guard_denied;
static service_memory_state_t memory_state={.minimum_free=UINT32_MAX};
static uint32_t service_free_memory(void) {
#if defined(MOC) && MOC == 1
    const micoMemInfo_t *m = MicoGetMemoryInfo();
    return m && m->free_memory > 0 ? (uint32_t)m->free_memory : 0;
#else
    return UINT32_MAX; /* Host simulators use the policy's injectable unit tests. */
#endif
}
/* Independent binary allocations retain the network/RTOS reserve. */
void *ph_wire_malloc(size_t bytes){
 uint32_t available=service_free_memory();
 bool denied=available<SERVICE_MEMORY_RESERVE+32u || bytes>available-SERVICE_MEMORY_RESERVE-32u;
 void *p=denied?NULL:malloc(bytes);
 if(!p&&wire_diag_ready){mico_rtos_lock_mutex(&wire_diag_lock);uint32_t *counter=denied?&wire_guard_denied:&wire_alloc_failed;if(*counter!=UINT32_MAX)++*counter;mico_rtos_unlock_mutex(&wire_diag_lock);}
 return p;
}
void ph_wire_free(void *p){free(p);}
/* Never called with command_lock held: admission precedes dispatch. */
static bool mp_cache_evict(void);
unsigned service_memory_claim(size_t input_bytes) {
    if (!ready) return 0;
    mico_rtos_lock_mutex(&service_lock);
    unsigned claim = memory_budget_claim(&memory_state, service_free_memory(), input_bytes);
    mico_rtos_unlock_mutex(&service_lock);
    /* Cache bytes are expendable, unlike mutation receipts. Do not hold the
     * snapshot lock while taking command_lock (dispatch takes them in reverse).
     * A low-heap cache must not permanently prevent login/body admission. */
    if (!claim && service_memory_cost(input_bytes) && mp_cache_evict()) {
        mico_rtos_lock_mutex(&service_lock);
        claim = memory_budget_claim(&memory_state, service_free_memory(), input_bytes);
        mico_rtos_unlock_mutex(&service_lock);
    }
    return claim;
}
void service_memory_release(unsigned claim) {
    if (!ready) return;
    mico_rtos_lock_mutex(&service_lock);
    memory_budget_release(&memory_state, claim);
    mico_rtos_unlock_mutex(&service_lock);
}
void service_memory_waiting(bool enter) {
    if (!ready) return;
    mico_rtos_lock_mutex(&service_lock);
    if (enter) memory_state.waiting++;
    else if (memory_state.waiting) memory_state.waiting--;
    mico_rtos_unlock_mutex(&service_lock);
}
void service_memory_snapshot(service_memory_state_t *out) {
    if (!ready) { memset(out, 0, sizeof(*out)); return; }
    mico_rtos_lock_mutex(&service_lock);
    uint32_t free_bytes = service_free_memory();
    if (free_bytes < memory_state.minimum_free) memory_state.minimum_free = free_bytes;
    *out = memory_state;
    if(wire_diag_ready){mico_rtos_lock_mutex(&wire_diag_lock);out->wire_alloc_failed=wire_alloc_failed;out->wire_guard_denied=wire_guard_denied;mico_rtos_unlock_mutex(&wire_diag_lock);}
    mico_rtos_unlock_mutex(&service_lock);
}
void service_memory_mqtt(unsigned pending, bool overflow, uint32_t delay_ms) {
    if (!ready) return;
    mico_rtos_lock_mutex(&service_lock);
    memory_state.mqtt_pending=pending;
    if (pending>memory_state.mqtt_peak) memory_state.mqtt_peak=pending;
    if (overflow && memory_state.mqtt_overflow!=UINT32_MAX) memory_state.mqtt_overflow++;
    if (delay_ms>memory_state.mqtt_max_delay) memory_state.mqtt_max_delay=delay_ms;
    mico_rtos_unlock_mutex(&service_lock);
}
void service_init(void) {
    if (ready) return;
    if (!service_lock_ready) service_lock_ready = mico_rtos_init_mutex(&service_lock)==kNoErr;
    if (!service_lock_ready) return;
    if(!wire_diag_ready)wire_diag_ready=mico_rtos_init_mutex(&wire_diag_lock)==kNoErr;
    if(!wire_diag_ready)return;
    ready = mico_rtos_init_mutex(&command_lock)==kNoErr;
    if (!ready) return;
    ota_job_init();
    service_event("info","system","boot");
}
bool service_control_begin(void) {
    if (!ready) return false;
    mico_rtos_lock_mutex(&command_lock);
    if (ota_job_active()) { mico_rtos_unlock_mutex(&command_lock); return false; }
    return true;
}
void service_control_end(void) { mico_rtos_unlock_mutex(&command_lock); }
static bool identifier(const char *s,size_t max) {
    size_t n=strlen(s); if (!n || n>=max) return false;
    for (size_t i=0;i<n;i++) if (!isalnum((unsigned char)s[i]) && s[i]!='_' && s[i]!='.' && s[i]!='-') return false;
    return true;
}
void service_event(const char *level,const char *module,const char *event) {
    if (!ready || !identifier(level,8) || !identifier(module,16) || !identifier(event,40)) return;
    service_record_t encoded={0};if(!log_encode(level,module,event,&encoded))return;
    mico_rtos_lock_mutex(&service_lock);
    service_record_t *r=&records[sequence%LOG_COUNT]; *r=encoded;
    r->sequence=++sequence; r->uptime=mico_rtos_get_time();
    time_t utc=time(NULL); if (utc>=1577836800) r->utc=(uint32_t)utc;
    mico_rtos_unlock_mutex(&service_lock);
    service_cache_changed(SERVICE_CACHE_STATUS);
}

#include "ota/update_mp.inc"
#include "service_mp.inc"
