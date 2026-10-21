#ifndef TEST_MQTT_MICO_H
#define TEST_MQTT_MICO_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uintptr_t mico_thread_t;
typedef uintptr_t mico_thread_arg_t;
typedef uint64_t mico_utc_time_ms_t;
typedef unsigned mico_mutex_t;
typedef struct { int free_memory; } micoMemInfo_t;
#define kNoErr 0
int mico_rtos_init_mutex(mico_mutex_t *);
int mico_rtos_deinit_mutex(mico_mutex_t *);
void mico_rtos_lock_mutex(mico_mutex_t *);
void mico_rtos_unlock_mutex(mico_mutex_t *);
bool mico_rtos_is_current_thread(mico_thread_t *);
uint32_t mico_rtos_get_time(void);
const micoMemInfo_t *MicoGetMemoryInfo(void);
#endif

void mico_thread_msleep(unsigned);
