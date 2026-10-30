#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned mico_mutex_t,mico_semaphore_t;
static mico_mutex_t command_lock;static int fail_init,fail_set;static unsigned initialized,destroyed;
#define kNoErr 0
#define MICO_WAIT_FOREVER UINT32_MAX
static int mico_rtos_init_semaphore(mico_semaphore_t*s,unsigned count){assert(count==1);if(fail_init)return -1;initialized++;*s=0;return 0;}
static int mico_rtos_set_semaphore(mico_semaphore_t*s){if(fail_set)return -1;assert(!*s);*s=1;return 0;}
static int mico_rtos_deinit_semaphore(mico_semaphore_t*s){destroyed++;*s=0;return 0;}
static int mico_rtos_get_semaphore(mico_semaphore_t*s,uint32_t wait){(void)wait;if(!*s)return -1;*s=0;return 0;}
static void mico_rtos_lock_mutex(mico_mutex_t*s){assert(!*s);*s=1;}
static void mico_rtos_unlock_mutex(mico_mutex_t*s){assert(*s);*s=0;}
#include "common/service_lock.inc"
int main(void){
 fail_init=1;assert(!service_command_gate_init()&&!initialized&&!destroyed&&!command_gate_ready);
 fail_init=0;fail_set=1;assert(!service_command_gate_init()&&initialized==1&&destroyed==1&&!command_gate_ready);
 fail_set=0;assert(service_command_gate_init()&&initialized==2&&destroyed==1&&command_gate_ready);
 assert(service_command_gate_init()&&initialized==2);assert(service_command_enter(false));assert(!service_command_enter(false));service_command_leave();assert(service_command_enter(true));service_command_leave();
 puts("actual binary command admission: init failure/set cleanup/retry/idempotence/exclusion PASS");return 0;
}
