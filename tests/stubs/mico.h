#pragma once
#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int OSStatus;
typedef unsigned mico_mutex_t;
typedef uintptr_t mico_thread_arg_t;
typedef uint64_t mico_utc_time_ms_t;
#define UNUSED_PARAMETER(x) (void)(x)
#define kNoErr 0
#define kUnknownErr -1
#define MICO_APPLICATION_PRIORITY 1
extern uint32_t test_now;
static inline int mico_rtos_init_mutex(mico_mutex_t *m) {
  *m = 0;
  return 0;
}
static inline void mico_rtos_lock_mutex(mico_mutex_t *m) {
  assert(!*m);
  *m = 1;
}
static inline void mico_rtos_unlock_mutex(mico_mutex_t *m) {
  assert(*m);
  *m = 0;
}
static inline uint32_t mico_rtos_get_time(void) { return test_now; }
static inline void mico_thread_msleep(unsigned n) { test_now += n; }
static inline int mico_rtos_create_thread(void *a, int p, const char *n,
                                          void (*f)(mico_thread_arg_t),
                                          unsigned stack, int arg) {
  (void)a;
  (void)p;
  (void)n;
  (void)f;
  (void)stack;
  (void)arg;
  return 0;
}
static inline int MicoRandomNumberRead(void *out, int n) {
  memset(out, 0xab, n);
  return 0;
}
static inline int mico_time_get_utc_time_ms(uint64_t *out) {
  *out = 1700000000000ULL + test_now;
  return 0;
}
static inline int mico_time_set_utc_time_ms(uint64_t *out) {
  (void)out;
  return 0;
}
