#include "m1.h"

/* MK3080B aliases: SDA=GPIO8, SCL=GPIO7; UART1 stays on GPIO9/10.
 * Separate STOP-delimited writes and reads are required by SCD40. */
static mico_i2c_device_t sensor = {
    .port = MICO_I2C_1, .address = 0x62,
    .address_width = I2C_ADDRESS_WIDTH_7BIT,
    .speed_mode = I2C_STANDARD_SPEED_MODE
};
static bool initialized;
static unsigned phase;
static uint32_t due = 1000;

static OSStatus command(uint16_t value)
{
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.scd40.last_command = value;
    mico_rtos_unlock_mutex(&m1_lock);
    uint8_t bytes[2] = {value >> 8, value & 0xff};
    mico_i2c_message_t message;
    OSStatus err = MicoI2cBuildTxMessage(&message, bytes, sizeof(bytes), 1);
    if (err == kNoErr) err = MicoI2cTransfer(&sensor, &message, 1);
    return err;
}

static OSStatus read_words(uint16_t *words, unsigned count)
{
    uint8_t bytes[9];
    mico_i2c_message_t message;
    OSStatus err = MicoI2cBuildRxMessage(&message, bytes, count * 3, 1);
    if (err == kNoErr) err = MicoI2cTransfer(&sensor, &message, 1);
    if (err == kNoErr && !scd40_words(bytes, words, count)) {
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.scd40.crc_errors++;
        mico_rtos_unlock_mutex(&m1_lock);
        return kChecksumErr;
    }
    return err;
}

void m1_scd40_tick(void)
{
    uint32_t now = mico_rtos_get_time();
    if ((int32_t)(now - due) < 0) return;
    OSStatus err = kNoErr;
    uint16_t words[3];
    switch (phase) {
    case 0:
        if (initialized) { MicoI2cFinalize(&sensor); initialized = false; }
        err = MicoI2cInitialize(&sensor);
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.scd40.bus_initialized = err == kNoErr;
        mico_rtos_unlock_mutex(&m1_lock);
        if (err != kNoErr) break;
        initialized = true;
        /* Also handles an MCU-only reboot while the sensor is measuring. */
        err = command(0x3f86);
        if (err == kNoErr) { phase = 1; due = now + 500; }
        break;
    case 1:
        err = command(0x3682); /* serial number */
        if (err == kNoErr) { phase = 2; due = now + 2; }
        break;
    case 2:
        err = read_words(words, 3);
        if (err != kNoErr) break;
        mico_rtos_lock_mutex(&m1_lock);
        snprintf(m1_state.scd40.serial, sizeof(m1_state.scd40.serial),
                 "%04x%04x%04x", words[0], words[1], words[2]);
        mico_rtos_unlock_mutex(&m1_lock);
        err = command(0x21b1); /* periodic measurements, 5 seconds */
        if (err == kNoErr) {
            mico_rtos_lock_mutex(&m1_lock);
            m1_state.scd40.ready = true;
            m1_state.scd40.last_error = 0;
            mico_rtos_unlock_mutex(&m1_lock);
            phase = 3; due = now + 5000;
        }
        break;
    case 3:
        err = command(0xe4b8); /* data ready */
        if (err == kNoErr) { phase = 4; due = now + 2; }
        break;
    case 4:
        err = read_words(words, 1);
        if (err != kNoErr) break;
        if (!(words[0] & 0x7ff)) { phase = 3; due = now + 1000; break; }
        err = command(0xec05);
        if (err == kNoErr) { phase = 5; due = now + 2; }
        break;
    case 5:
        err = read_words(words, 3);
        if (err != kNoErr) break;
        if (!words[0]) { err = kStateErr; break; }
        mico_rtos_lock_mutex(&m1_lock);
        scd40_state_t *s = &m1_state.scd40;
        s->co2_ppm = words[0];
        s->temperature_milli_c = -45000 + (int32_t)((175000LL * words[1]) / 65535);
        s->humidity_milli_pct = (100000ULL * words[2]) / 65535;
        s->updated_ms = now; s->received = true; s->samples++; s->last_error = 0;
        mico_rtos_unlock_mutex(&m1_lock);
        phase = 3; due = now + 5000;
        break;
    }
    if (err != kNoErr) {
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.scd40.ready = false;
        m1_state.scd40.error_phase = phase;
        m1_state.scd40.last_error = err;
        m1_state.scd40.errors++;
        mico_rtos_unlock_mutex(&m1_lock);
        phase = 0; due = now + 5000;
    }
}
