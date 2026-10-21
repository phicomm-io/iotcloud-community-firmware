#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
typedef int OSStatus;
typedef unsigned mico_mutex_t;
typedef uintptr_t mico_thread_arg_t;
typedef uintptr_t mico_thread_t;
static inline bool mico_rtos_is_current_thread(mico_thread_t *thread){return *thread==1;}
#ifndef PHICOMM_HOST_CUSTOM_MEMORY
typedef struct { unsigned free_memory; } micoMemInfo_t;
static inline const micoMemInfo_t *MicoGetMemoryInfo(void){static const micoMemInfo_t m={64000};return &m;}
#endif
typedef uint32_t mico_time_t;
typedef int mico_gpio_t, WiFiEvent;
#define maxNameLen 64
#define UNUSED_PARAMETER(x) (void)(x)
#define MICO_APPLICATION_PRIORITY 1
#define kNoErr 0
#define kUnknownErr -1
#define kParamErr -2
#define kNoMemoryErr -3
#define kDeletedErr -4
#define kConnectionErr -5
#define require(x,label) do { if(!(x)) goto label; } while(0)
#define require_action(x,label,act) do { if(!(x)){act;goto label;} } while(0)
#define require_string(x,label,str) require(x,label)
#define require_noerr(x,label) require((x)==0,label)
#define require_noerr_action(x,label,act) require_action((x)==0,label,act)
#define require_noerr_string(x,label,str) require_noerr(x,label)
#define custom_log(...) ((void)0)
#define custom_log_trace(...) ((void)0)
uint32_t mico_rtos_get_time(void);
void mico_thread_msleep(unsigned);
#define mico_rtos_thread_msleep mico_thread_msleep
void mico_rtos_thread_sleep(unsigned);
static inline int mico_rtos_init_mutex(mico_mutex_t *m){*m=0;return 0;}
static inline void mico_rtos_lock_mutex(mico_mutex_t *m){assert(!*m);*m=1;}
static inline void mico_rtos_unlock_mutex(mico_mutex_t *m){assert(*m);*m=0;}
static inline void mico_rtos_suspend_all_thread(void){}
static inline void mico_rtos_resume_all_thread(void){}
static inline void mico_rtos_delete_thread(void *p){(void)p;}
int mico_rtos_create_thread(void *,int,const char *,void (*)(mico_thread_arg_t),unsigned,int);
typedef struct { void *items[10]; unsigned head,count; } host_queue;
typedef host_queue *mico_queue_t;
typedef int mico_worker_thread_t,mico_timer_t;
typedef void (*mico_thread_function_t)(mico_thread_arg_t);
static inline int mico_rtos_init_queue(mico_queue_t *q,const char *name,unsigned size,unsigned count){(void)name;(void)size;(void)count;*q=calloc(1,sizeof(**q));return *q?0:-1;}
static inline bool mico_rtos_is_queue_empty(mico_queue_t *q){return !(*q)->count;}
static inline bool mico_rtos_is_queue_full(mico_queue_t *q){return (*q)->count==10;}
static inline int mico_rtos_pop_from_queue(mico_queue_t *q,void *out,unsigned wait){(void)wait;if(!(*q)->count)return -1;*(void **)out=(*q)->items[(*q)->head];(*q)->head=((*q)->head+1)%10;--(*q)->count;return 0;}
static inline int mico_rtos_push_to_queue(mico_queue_t *q,void *in,unsigned wait){(void)wait;if((*q)->count==10)return -1;(*q)->items[((*q)->head+(*q)->count)%10]=*(void **)in;++(*q)->count;return 0;}
static inline void mico_rtos_deinit_queue(mico_queue_t *q){free(*q);*q=NULL;}
static inline int mico_rtos_create_worker_thread(mico_worker_thread_t *w,int p,unsigned stack,unsigned count){(void)w;(void)p;(void)stack;(void)count;return 0;}
static inline int mico_rtos_send_asynchronous_event(mico_worker_thread_t *w,OSStatus (*f)(void *),void *arg){(void)w;f(arg);return 0;}
static inline void mico_start_timer(mico_timer_t *t){(void)t;}
static inline void mico_stop_timer(mico_timer_t *t){(void)t;}
typedef struct { int is_connected; } LinkStatusTypeDef;
static inline void micoWlanGetLinkStatus(LinkStatusTypeDef *s){s->is_connected=1;}
static inline void mico_time_get_time(mico_time_t *out){*out=mico_rtos_get_time();}
static inline int mico_system_context_update(void *p){(void)p;return 0;}
static inline void MicoSystemReboot(void){}
