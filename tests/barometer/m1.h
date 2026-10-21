#pragma once
#include "barometer.h"
#include <string.h>

typedef int OSStatus;
typedef int mico_mutex_t;
enum { kNoErr=0, kTimeoutErr=-6722, kStateErr=-1, MICO_I2C_1=1,
       I2C_ADDRESS_WIDTH_7BIT=7, I2C_STANDARD_SPEED_MODE=100 };
typedef struct { unsigned port,address,address_width,speed_mode; } mico_i2c_device_t;
typedef struct { const uint8_t *tx; uint8_t *rx; unsigned tx_len,rx_len; } mico_i2c_message_t;
typedef struct { barometer_state_t barometer; } m1_state_t;
extern mico_mutex_t m1_lock;
extern m1_state_t m1_state;
uint32_t mico_rtos_get_time(void);
void mico_thread_msleep(uint32_t);
void mico_rtos_lock_mutex(mico_mutex_t *);
void mico_rtos_unlock_mutex(mico_mutex_t *);
OSStatus MicoI2cInitialize(mico_i2c_device_t *);
OSStatus MicoI2cBuildCombinedMessage(mico_i2c_message_t *, const void *, void *, uint16_t, uint16_t, uint16_t);
OSStatus MicoI2cBuildTxMessage(mico_i2c_message_t *, const void *, uint16_t, uint16_t);
OSStatus MicoI2cBuildRxMessage(mico_i2c_message_t *, void *, uint16_t, uint16_t);
OSStatus MicoI2cTransfer(mico_i2c_device_t *, mico_i2c_message_t *, uint16_t);
