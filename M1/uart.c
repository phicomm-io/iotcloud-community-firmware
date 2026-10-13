#include "m1.h"
#include <time.h>

typedef struct { uint8_t kind, level, frame[12]; } uart_command_t;
static mico_queue_t tx_queue;
static bool queue_ready;

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

static void pwm_update(unsigned level, unsigned mode, bool blink)
{
    static const float duty[5] = { 0.5f, 3.0f, 7.0f, 13.0f, 20.0f };
    /* MK3080B: PWM4 = GPIO13, PWM5 = GPIO14. No generic board LED pins. */
    MicoPwmStop(MICO_PWM_4); MicoPwmStop(MICO_PWM_5);
    MicoPwmInitialize(MICO_PWM_4, 50000, duty[level]);
    MicoPwmInitialize(MICO_PWM_5, 50000, duty[level]);
    if (mode == 2) MicoPwmStart(MICO_PWM_4);
    if (mode == 1 || mode == 2 || (mode == 3 && blink)) MicoPwmStart(MICO_PWM_5);
}

static void uart_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    uint8_t storage[128], frame[20];
    ring_buffer_t ring;
    ring_buffer_init(&ring, storage, sizeof(storage));
    mico_uart_config_t config = { 115200, DATA_WIDTH_8BIT, NO_PARITY,
                                  STOP_BITS_1, FLOW_CONTROL_DISABLED };
    if (MicoUartInitialize(MICO_UART_1, &config, &ring) != kNoErr) {
        uart_error(); queue_ready = false; mico_rtos_delete_thread(NULL); return;
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
    unsigned old_level = 255, old_mode = 255, old_blink = 255;
    for (;;) {
        uint32_t now = mico_rtos_get_time();
        if ((int32_t)(now - next_query) >= 0) {
            m1_query_frame(frame); transmit(frame); next_query = now + M1_POLL_MS;
        }
        uart_command_t command;
        if (mico_rtos_pop_from_queue(&tx_queue, &command, 0) == kNoErr) {
            if (command.kind == 2) {
                m1_config_get(&cfg);
                m1_brightness_frame(command.frame, command.level, cfg.last_on);
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
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.discarded_bytes = decoder.discarded;
        unsigned level = m1_state.brightness_requested;
        unsigned mode = m1_state.wifi_connected ? 1 : (m1_state.ap_active ? 2 : 3);
        mico_rtos_unlock_mutex(&m1_lock);
        unsigned blink = (now / 500) % 2;
        if (level != old_level || mode != old_mode || (mode == 3 && blink != old_blink)) {
            pwm_update(level, mode, blink);
            old_level = level; old_mode = mode; old_blink = blink;
        }
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

