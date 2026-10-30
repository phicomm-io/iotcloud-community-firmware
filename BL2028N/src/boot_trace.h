/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Opt-in diagnostics on programming TX/P11 (UART1), away from the meter's
 * UART2 RX/P1. Product initialization also routes ordinary logs to UART1. */
#if PH_BL_BOOT_TRACE && !defined(PH_HOST_TEST)
#include "../../../beken378/func/user_driver/BkDriverUart.h"
#include "task.h"
extern void set_printf_port(unsigned char);
static inline void ph_boot_result(const char *stage, int result) {
    TaskStatus_t task;
    vTaskGetInfo(NULL,&task,pdTRUE,eInvalid);
    bk_printf("[S30-BOOT] %s rc=%d ms=%u heap=%u stack=%u task=%s\r\n",stage,result,
              (unsigned)rtos_get_time(),(unsigned)xPortGetFreeHeapSize(),
              (unsigned)task.usStackHighWaterMark*sizeof(StackType_t),task.pcTaskName);
}
static inline void ph_boot_start(void) {
    bk_uart_config_t config={4800,3,2,0,0,0};
    int result=bk_uart_initialize(BK_UART_1,&config,NULL);
    set_printf_port(1);
    ph_boot_result("iotcloud S30-V1B app 4800E",result);
}
#define PH_BOOT_START() ph_boot_start()
#define PH_BOOT_RESULT(stage,result) ph_boot_result(stage,result)
#define PH_BOOT_PULSE(stage) do { \
    static uint32_t last; uint32_t tick=rtos_get_time(); \
    if ((uint32_t)(tick-last)>=1000) {last=tick;ph_boot_result(stage,0);} \
} while(0)
#else
#define PH_BOOT_START() ((void)0)
#define PH_BOOT_RESULT(stage,result) ((void)0)
#define PH_BOOT_PULSE(stage) ((void)0)
#endif
#define PH_BOOT_STAGE(stage) PH_BOOT_RESULT(stage,0)
