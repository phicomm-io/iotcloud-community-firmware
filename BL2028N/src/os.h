/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* The shared protocol owners use the OpenBeken/Beken RTOS directly. */
#ifdef PH_HOST_TEST
#include "os_host.h"
#define ph_os_load(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define ph_os_store(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
static inline void ph_os_cas32(uint32_t *p,uint32_t *expected,uint32_t next) {
    __atomic_compare_exchange_n(p,expected,next,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
#else
#include "../../new_common.h"
#include "../../hal/hal_generic.h"
typedef beken_thread_t ph_os_thread;
typedef beken_mutex_t ph_os_mutex;
static inline uint32_t ph_os_now(void) { return rtos_get_time(); }
static inline void ph_os_sleep(unsigned ms) { rtos_delay_milliseconds(ms); }
static inline bool ph_os_current(ph_os_thread *thread) { return rtos_is_current_thread(thread); }
static inline int ph_os_mutex_init(ph_os_mutex *mutex) { return rtos_init_mutex(mutex); }
static inline void ph_os_mutex_free(ph_os_mutex *mutex) { rtos_deinit_mutex(mutex); }
static inline void ph_os_lock(ph_os_mutex *mutex) { rtos_lock_mutex(mutex); }
static inline void ph_os_unlock(ph_os_mutex *mutex) { rtos_unlock_mutex(mutex); }
static inline size_t ph_os_free_heap(void) { return xPortGetFreeHeapSize(); }
/* ARM968 has no LDREX/STREX or libatomic. Publication across its single-core
 * RTOS tasks uses short critical sections; no allocation or socket I/O here. */
#define ph_os_load(p) ({ rtos_enter_critical(); __typeof__(*(p)) ph_value_=*(p); __asm__ volatile("" ::: "memory"); rtos_exit_critical(); ph_value_; })
#define ph_os_store(p,v) do { rtos_enter_critical(); __asm__ volatile("" ::: "memory"); *(p)=(v); rtos_exit_critical(); } while(0)
static inline void ph_os_cas32(uint32_t *p,uint32_t *expected,uint32_t next) {
    rtos_enter_critical();
    if(*p==*expected)*p=next;else *expected=*p;
    __asm__ volatile("" ::: "memory");rtos_exit_critical();
}
#endif
