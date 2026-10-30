/* POSIX binding for the same shared service owners used on BK7231N. */
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef pthread_t beken_thread_t;
typedef pthread_mutex_t beken_mutex_t;
typedef uintptr_t beken_thread_arg_t;
typedef beken_thread_t ph_os_thread;
typedef beken_mutex_t ph_os_mutex;
#define BEKEN_APPLICATION_PRIORITY 0
#define USER_SW_VER "host-shared-services"
extern int ph_host_http_port;
uint32_t rtos_get_time(void);
int rtos_init_mutex(beken_mutex_t *);
void rtos_lock_mutex(beken_mutex_t *);
void rtos_unlock_mutex(beken_mutex_t *);
int rtos_create_thread(beken_thread_t *,int,const char *,void (*)(beken_thread_arg_t),int,beken_thread_arg_t);
void rtos_delete_thread(beken_thread_t *);
void rtos_delay_milliseconds(unsigned);
size_t xPortGetFreeHeapSize(void);
static inline uint32_t ph_os_now(void){return rtos_get_time();}
static inline void ph_os_sleep(unsigned ms){rtos_delay_milliseconds(ms);}
static inline bool ph_os_current(ph_os_thread *t){return pthread_equal(pthread_self(),*t)!=0;}
static inline int ph_os_mutex_init(ph_os_mutex *m){return pthread_mutex_init(m,NULL);}
static inline void ph_os_mutex_free(ph_os_mutex *m){pthread_mutex_destroy(m);}
static inline void ph_os_lock(ph_os_mutex *m){pthread_mutex_lock(m);}
static inline void ph_os_unlock(ph_os_mutex *m){pthread_mutex_unlock(m);}
static inline size_t ph_os_free_heap(void){return xPortGetFreeHeapSize();}
