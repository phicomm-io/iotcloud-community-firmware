#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
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
static inline const micoMemInfo_t *MicoGetMemoryInfo(void){
 static __thread micoMemInfo_t info;info.free_memory=test_heap_free;return &info;
}
#ifdef TEST_ACTUAL_OTA_WORKER
#include <stddef.h>
typedef intptr_t mico_thread_arg_t;
#define MICO_APPLICATION_PRIORITY 1
int mico_rtos_create_thread(void *,int,const char *,void (*)(mico_thread_arg_t),unsigned,mico_thread_arg_t);
void mico_rtos_delete_thread(void *);
void test_run_worker(void);
#endif
