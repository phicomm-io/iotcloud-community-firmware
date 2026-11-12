#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <semaphore.h>
#include <errno.h>
typedef sem_t mico_semaphore_t;
static inline int mico_rtos_deinit_semaphore(mico_semaphore_t *s){return sem_destroy(s);}
#define MICO_WAIT_FOREVER UINT32_MAX
static inline int mico_rtos_init_semaphore(mico_semaphore_t *s,int n){(void)n;return sem_init(s,0,0);}
static inline int mico_rtos_set_semaphore(mico_semaphore_t *s){return sem_post(s);}
static inline int mico_rtos_get_semaphore(mico_semaphore_t *s,uint32_t timeout){if(!timeout)return sem_trywait(s);int rc;do{rc=sem_wait(s);}while(rc&&errno==EINTR);return rc;}
typedef pthread_mutex_t mico_mutex_t;
#define kNoErr 0
static inline int mico_rtos_init_mutex(mico_mutex_t *m){return pthread_mutex_init(m,NULL);}
static inline void mico_rtos_lock_mutex(mico_mutex_t *m){assert(!pthread_mutex_lock(m));}
static inline void mico_rtos_unlock_mutex(mico_mutex_t *m){assert(!pthread_mutex_unlock(m));}
extern uint32_t test_clock_ms;
static inline uint32_t mico_rtos_get_time(void){return test_clock_ms;}

/* Compile the real MOC admission/allocation branch, not its unlimited host fallback. */
typedef struct {uint32_t free_memory;} micoMemInfo_t;
extern uint32_t test_heap_free;
extern size_t test_heap_live_bytes;
extern bool test_heap_dynamic;
static inline const micoMemInfo_t *MicoGetMemoryInfo(void){
 static __thread micoMemInfo_t info;info.free_memory=test_heap_dynamic ? (test_heap_live_bytes<test_heap_free ? test_heap_free-test_heap_live_bytes : 0) : test_heap_free;return &info;
}
#ifdef TEST_ACTUAL_OTA_WORKER
#include <stddef.h>
typedef intptr_t mico_thread_arg_t;
#define MICO_APPLICATION_PRIORITY 1
int mico_rtos_create_thread(void *,int,const char *,void (*)(mico_thread_arg_t),unsigned,mico_thread_arg_t);
void mico_rtos_delete_thread(void *);
void test_run_worker(void);
#endif
