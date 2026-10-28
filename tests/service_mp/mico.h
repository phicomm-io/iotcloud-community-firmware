#pragma once
#include <stdint.h>
typedef unsigned mico_mutex_t;
#define kNoErr 0
static inline int mico_rtos_init_mutex(mico_mutex_t *m){*m=0;return 0;}
static inline void mico_rtos_lock_mutex(mico_mutex_t *m){(void)m;}
static inline void mico_rtos_unlock_mutex(mico_mutex_t *m){(void)m;}
static inline uint32_t mico_rtos_get_time(void){return 123000;}
