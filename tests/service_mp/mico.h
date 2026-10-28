#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
typedef pthread_mutex_t mico_mutex_t;
#define kNoErr 0
static inline int mico_rtos_init_mutex(mico_mutex_t *m){return pthread_mutex_init(m,NULL);}
static inline void mico_rtos_lock_mutex(mico_mutex_t *m){assert(!pthread_mutex_lock(m));}
static inline void mico_rtos_unlock_mutex(mico_mutex_t *m){assert(!pthread_mutex_unlock(m));}
static inline uint32_t mico_rtos_get_time(void){return 123000;}
