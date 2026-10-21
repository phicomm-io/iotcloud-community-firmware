#include "mico.h"
#include "status_guard.h"
static mico_mutex_t state_lock;
static bool ready;
bool tc1_status_guard_init(void)
{ ready=mico_rtos_init_mutex(&state_lock)==kNoErr;return ready; }
bool tc1_status_guard_ready(void) { return ready; }
void tc1_status_lock(void) { if(ready)mico_rtos_lock_mutex(&state_lock); }
void tc1_status_unlock(void) { if(ready)mico_rtos_unlock_mutex(&state_lock); }
bool tc1_status_begin(void)
{
    if(!ready)return false;
    tc1_status_lock();
    if(tc1_config_changing()){tc1_status_unlock();return false;}
    return true;
}
