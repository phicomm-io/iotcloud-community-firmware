#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int OSStatus;
typedef uintptr_t mico_thread_arg_t;
#define kNoErr 0
#define UNUSED_PARAMETER(x) (void)(x)
#define custom_log(...) ((void)0)
uint32_t mico_rtos_get_time(void);
void mico_rtos_enter_critical(void);
void mico_rtos_exit_critical(void);
void mico_thread_msleep(unsigned);
int MicoWdgInitialize(uint32_t);
void MicoWdgReload(void);
int mico_rtos_create_thread(void *, int, const char *, void (*)(mico_thread_arg_t), unsigned, int);
#define MICO_PARTITION_OTA_TEMP 1
int MicoFlashErase(int, uint32_t, uint32_t);
int MicoFlashRead(int, uint32_t *, void *, uint32_t);
