#include <m1.h>
#include "bmp3_vendor/bmp3.h"

/* Optional sensor on I2C1: SDA=GPIO8, SCL=GPIO7. Never reset/finalize
 * the shared peripheral here; SCD40 uses the same bus on this task. */
static mico_i2c_device_t bus = {
    .port = MICO_I2C_1, .address = 0x76,
    .address_width = I2C_ADDRESS_WIDTH_7BIT,
    .speed_mode = I2C_STANDARD_SPEED_MODE
};
static struct bmp3_dev dev;
static bool configured;
static uint32_t due = 3000, last_sample_at;
static OSStatus bus_error;

static BMP3_INTF_RET_TYPE read_regs(uint8_t reg, uint8_t *data, uint32_t length, void *ctx)
{
    (void)ctx;
    mico_i2c_message_t message;
    bus_error = MicoI2cBuildCombinedMessage(&message, &reg, data, 1, length, 1);
    if (bus_error == kNoErr) bus_error = MicoI2cTransfer(&bus, &message, 1);
    if (bus_error != kNoErr) {
        /* ROM compatibility: register pointer write followed by a STOP/read. */
        bus_error = MicoI2cBuildTxMessage(&message, &reg, 1, 1);
        if (bus_error == kNoErr) bus_error = MicoI2cTransfer(&bus, &message, 1);
        if (bus_error == kNoErr) bus_error = MicoI2cBuildRxMessage(&message, data, length, 1);
        if (bus_error == kNoErr) bus_error = MicoI2cTransfer(&bus, &message, 1);
    }
    /* The vendor normal-mode validator indexes an 18-entry ODR table.
     * Reject invalid register contents before they reach that validator. */
    if (bus_error == kNoErr && reg == BMP3_REG_OSR && length == 4 &&
        (data[1] & BMP3_ODR_MSK) > BMP3_ODR_0_001_HZ) bus_error = kStateErr;
    return bus_error == kNoErr ? BMP3_OK : BMP3_E_COMM_FAIL;
}

static BMP3_INTF_RET_TYPE write_regs(uint8_t reg, const uint8_t *data, uint32_t length, void *ctx)
{
    (void)ctx;
    uint8_t bytes[32];
    if (length >= sizeof(bytes)) return BMP3_E_INVALID_LEN;
    bytes[0] = reg;
    memcpy(bytes + 1, data, length);
    mico_i2c_message_t message;
    bus_error = MicoI2cBuildTxMessage(&message, bytes, length + 1, 1);
    if (bus_error == kNoErr) bus_error = MicoI2cTransfer(&bus, &message, 1);
    return bus_error == kNoErr ? BMP3_OK : BMP3_E_COMM_FAIL;
}

static void delay_us(uint32_t us, void *ctx)
{
    (void)ctx;
    mico_thread_msleep((us + 999) / 1000);
}

static int8_t detect(void)
{
    int8_t rc = BMP3_E_DEV_NOT_FOUND;
    for (unsigned address = 0x76; address <= 0x77; ++address) {
        uint8_t id = 0;
        bus.address = address;
        bus_error = MicoI2cInitialize(&bus);
        if (bus_error != kNoErr) { rc = BMP3_E_COMM_FAIL; continue; }
        rc = read_regs(BMP3_REG_CHIP_ID, &id, 1, NULL);
        if (rc != BMP3_OK) continue;
        if (!m1_barometer_model(id)) { rc = BMP3_E_DEV_NOT_FOUND; continue; }
        /* Only configure a positively identified supported chip. */
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.barometer.chip_id = id;
        m1_state.barometer.address = address;
        m1_state.barometer.received = false;
        mico_rtos_unlock_mutex(&m1_lock);
        memset(&dev, 0, sizeof(dev));
        dev.intf = BMP3_I2C_INTF;
        dev.intf_ptr = &bus;
        dev.read = read_regs; dev.write = write_regs; dev.delay_us = delay_us;
        rc = bmp3_init(&dev);
        if (rc != BMP3_OK) continue;
        struct bmp3_settings settings = {0};
        settings.press_en = settings.temp_en = BMP3_ENABLE;
        settings.odr_filter.press_os = BMP3_OVERSAMPLING_8X;
        settings.odr_filter.temp_os = BMP3_OVERSAMPLING_2X;
        settings.odr_filter.odr = BMP3_ODR_1_5_HZ;
        /* One register per vendor call avoids its interleaved burst-write
         * path (which includes an unused trailing byte in this API version). */
        rc = bmp3_set_sensor_settings(BMP3_SEL_PRESS_EN | BMP3_SEL_TEMP_EN, &settings, &dev);
        if (rc == BMP3_OK) rc = bmp3_set_sensor_settings(BMP3_SEL_PRESS_OS | BMP3_SEL_TEMP_OS, &settings, &dev);
        if (rc == BMP3_OK) rc = bmp3_set_sensor_settings(BMP3_SEL_ODR, &settings, &dev);
        settings.op_mode = BMP3_MODE_NORMAL;
        if (rc == BMP3_OK) rc = bmp3_set_op_mode(&settings, &dev);
        if (rc == BMP3_OK) return rc;
    }
    return rc;
}

void m1_barometer_tick(void)
{
    uint32_t now = mico_rtos_get_time();
    if ((int32_t)(now - due) < 0) return;
    due = now + 1000;
    bus_error = kNoErr;
    int8_t rc;
    if (!configured) {
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.barometer.attempts++;
        m1_state.barometer.ready = false;
        m1_state.barometer.received = false;
        m1_state.barometer.chip_id = m1_state.barometer.address = 0;
        mico_rtos_unlock_mutex(&m1_lock);
        rc = detect();
        configured = rc == BMP3_OK;
        last_sample_at = now;
    } else {
        struct bmp3_status status = {0};
        rc = bmp3_get_status(&status, &dev);
        if (rc == BMP3_OK && (status.err.fatal || status.err.cmd || status.err.conf))
            rc = BMP3_E_CONFIGURATION_ERR;
        if (rc == BMP3_OK && status.sensor.drdy_press && status.sensor.drdy_temp) {
            struct bmp3_data data = {0};
            rc = bmp3_get_sensor_data(BMP3_PRESS_TEMP, &data, &dev);
            /* A vendor range warning is not a valid (clamped) measurement. */
            if (rc == BMP3_OK) {
                mico_rtos_lock_mutex(&m1_lock);
                m1_state.barometer.pressure_centi_pa = data.pressure;
                m1_state.barometer.updated_ms = mico_rtos_get_time();
                m1_state.barometer.received = true;
                m1_state.barometer.samples++;
                mico_rtos_unlock_mutex(&m1_lock);
                last_sample_at = now;
            }
        }
        if (rc == BMP3_OK && (uint32_t)(now - last_sample_at) >= M1_BAROMETER_STALE_MS)
            rc = BMP3_E_CMD_EXEC_FAILED;
    }
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.barometer.ready = configured && rc == BMP3_OK;
    m1_state.barometer.last_error = rc == BMP3_E_COMM_FAIL && bus_error != kNoErr ? bus_error : rc;
    if (rc != BMP3_OK) m1_state.barometer.errors++;
    mico_rtos_unlock_mutex(&m1_lock);
    if (rc != BMP3_OK) { configured = false; due = now + 10000; }
}
