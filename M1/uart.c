#include "../common/watchdog.h"
#include "m1.h"
#include "indicator.h"
#include <time.h>

typedef struct { uint8_t kind, level, frame[12]; } uart_command_t;
static mico_queue_t tx_queue;
static bool queue_ready;

static unsigned effective_level(unsigned desired)
{
    unsigned cap = feature_light_cap(255) * 4u / 255u;
    return desired < cap ? desired : cap;
}

bool m1_set_brightness(unsigned level)
{
    if (level > 4 || !queue_ready) return false;
    uart_command_t command = { .kind = 2, .level = level };
    return mico_rtos_push_to_queue(&tx_queue, &command, 0) == kNoErr;
}

bool m1_uart_clock(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{
    if (!queue_ready || y < 2020 || y > 2099) return false;
    uart_command_t command = { .kind = 3 };
    m1_clock_frame(command.frame, y, mo, d, h, mi, s);
    return mico_rtos_push_to_queue(&tx_queue, &command, 0) == kNoErr;
}

static void uart_error(void)
{
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.uart_errors++;
    mico_rtos_unlock_mutex(&m1_lock);
}

static bool transmit(const uint8_t *frame)
{
    if (MicoUartSend(MICO_UART_1, frame, 12) == kNoErr) return true;
    uart_error();
    return false;
}

static void indicator_output(mico_pwm_t pwm, mico_gpio_t gpio, float duty, bool on)
{
    MicoPwmStop(pwm);
    if (on) {
        MicoPwmInitialize(pwm, 50000, duty);
        MicoPwmStart(pwm);
    } else {
        /* PWM stop does not specify an inactive output level. Drive it low. */
        MicoGpioInitialize(gpio, OUTPUT_PUSH_PULL);
        MicoGpioOutputLow(gpio);
    }
}

static void pwm_update(unsigned level, bool wifi_on, bool error_on)
{
    static const float duty[5] = { 0.5f, 3.0f, 7.0f, 13.0f, 20.0f };
    /* MK3080B PWM4/GPIO13 drives the error mark; PWM5/GPIO14
     * drives Wi-Fi, as identified by the user on the physical M1. */
    indicator_output(MICO_PWM_5, MICO_GPIO_14, duty[level], level && wifi_on);
    indicator_output(MICO_PWM_4, MICO_GPIO_13, duty[level], level && error_on);
}

static void uart_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    uint8_t storage[128], frame[20];
    bool reset_seen = false;
    ring_buffer_t ring;
    ring_buffer_init(&ring, storage, sizeof(storage));
    mico_uart_config_t config = { 115200, DATA_WIDTH_8BIT, NO_PARITY,
                                  STOP_BITS_1, FLOW_CONTROL_DISABLED };
    while (MicoUartInitialize(MICO_UART_1, &config, &ring) != kNoErr) {
        uart_error(); queue_ready = false;
        watchdog_beat(WATCHDOG_HARDWARE);
        mico_thread_msleep(3000);
    }
    m1_config_t cfg;
    m1_config_get(&cfg);
    m1_brightness_frame(frame, cfg.brightness, cfg.last_on);
    transmit(frame);
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.uart_ready = true;
    mico_rtos_unlock_mutex(&m1_lock);
    queue_ready = true;
    uint32_t next_query = mico_rtos_get_time();
    m1_decoder_t decoder = {0};
    unsigned old_level = 255, old_mode = 255, old_error = 255, old_wifi = 255;
    uint32_t mode_since = 0;
    unsigned applied_display = cfg.brightness;
    for (;;) {
        uint32_t now = mico_rtos_get_time();
        if ((int32_t)(now - next_query) >= 0) {
            m1_query_frame(frame); transmit(frame); next_query = now + M1_POLL_MS;
        }
        uart_command_t command;
        if (mico_rtos_pop_from_queue(&tx_queue, &command, 0) == kNoErr) {
            if (command.kind == 2) {
                m1_config_get(&cfg);
                m1_brightness_frame(command.frame, effective_level(command.level), cfg.last_on);
                if (transmit(command.frame)) {
                    for (unsigned attempt = 0; attempt < 3; attempt++) {
                        m1_config_get(&cfg);
                        if (cfg.brightness == command.level) break;
                        cfg.brightness = command.level;
                        if (command.level) cfg.last_on = command.level;
                        if (m1_config_save(&cfg)) break;
                        if (attempt == 2) uart_error();
                    }
                    mico_rtos_lock_mutex(&m1_lock);
                    m1_state.brightness_requested = command.level;
                    mico_rtos_unlock_mutex(&m1_lock);
                }
            } else transmit(command.frame);
        }
        /* Bounded receive work: noise cannot starve queries or queued commands. */
        for (unsigned i = 0; i < 32; i++) {
            uint8_t byte;
            if (MicoUartRecv(MICO_UART_1, &byte, 1, i ? 0 : 10) != kNoErr) break;
            if (!m1_decode_byte(&decoder, byte, frame)) continue;
            if (frame[1] == 0x18) {
                static const uint8_t ack[12] = {0x23,0x16,0x00,0x10,0x14,0x00,0x03,0x10,0xf1,0xff,0xff,0x21};
                transmit(ack);
                if (!reset_seen) { reset_seen = true; m1_reset(false); }
                continue;
            }
            mico_rtos_lock_mutex(&m1_lock);
            memcpy(m1_state.last_frame, frame, 20);
            m1_state.rx_frames++;
            if (!m1_parse_sample(frame, mico_rtos_get_time(), &m1_state.sample)) {
                if (frame[1] == 0x0f && frame[2] <= 1) {
                    m1_state.brightness_requested = frame[2] ? cfg.last_on : 0;
                } else m1_state.unknown_frames++;
            }
            mico_rtos_unlock_mutex(&m1_lock);
        }
        m1_config_get(&cfg);
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.discarded_bytes = decoder.discarded;
        unsigned level = m1_state.brightness_requested;
        unsigned mode = m1_indicator_mode(cfg.ssid[0] != 0, m1_state.wifi_connected, cfg.mqtt_host[0] != 0,
            m1_state.mqtt_connected && m1_state.mqtt_status == MQTT_READY);
        mico_rtos_unlock_mutex(&m1_lock);
        level = effective_level(level);
        if (level != applied_display) {
            m1_brightness_frame(frame, level, cfg.last_on);
            if (transmit(frame)) applied_display = level;
        }
        if (mode != old_mode) mode_since = now;
        uint32_t elapsed = (uint32_t)(now - mode_since);
        bool wifi_connected = m1_indicator_wifi(mode, elapsed);
        bool error_on = m1_indicator_error(mode, elapsed);
        if (level != old_level || wifi_connected != old_wifi || error_on != old_error) {
            pwm_update(level, wifi_connected, error_on);
            old_level = level; old_wifi = wifi_connected; old_error = error_on;
        }
        old_mode = mode;
        m1_scd40_tick();
        m1_barometer_tick();
        watchdog_beat(WATCHDOG_HARDWARE);
        mico_thread_msleep(10);
    }
}

OSStatus m1_uart_start(void)
{
    OSStatus err = mico_rtos_init_queue(&tx_queue, "m1_uart_tx", sizeof(uart_command_t), 10);
    if (err != kNoErr) return err;
    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "m1_uart", uart_worker, 4096, 0);
    return err;
}

