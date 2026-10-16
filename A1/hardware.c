#include "../common/automation.h"
#include "a1.h"
#include "ring_feedback.h"
#include "../common/indicator.h"
#include "../common/gesture.h"
#if !defined(CONFIG_CPU_MX1290) || !defined(MOC) || MOC != 1
#error "A1 hardware requires the MK3080B MX1290 MOC target"
#endif

/* zA1 v0.3.1 static analysis; SDK aliases are NOT physical GPIO numbers. */
static mico_i2c_device_t panel = {
    .port = MICO_I2C_1, .address = 0x08,
    .address_width = I2C_ADDRESS_WIDTH_7BIT, .speed_mode = I2C_STANDARD_SPEED_MODE
};

/* Run every instruction and literal from SRAM. Clock: MK3080B 125 MHz.
 * Packed colors are 0xGGRRBB: wire bytes B,R,G, each MSB first.
 * Loop shapes/counts match the observed sender; physical timing still needs DUT proof.
 * Save PRIMASK rather than unconditionally enabling interrupts at the end. */
__attribute__((section(".fast"), noinline, optimize("O0")))
static void ring_send(const uint32_t *colors)
{
    uint32_t mask;
    __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (mask) : : "memory");
    volatile uint32_t *port = (volatile uint32_t *)0x40001000u;
    uint32_t other = *port, high = other | 0x1000u, low = other & ~0x1000u;
    uint16_t delay;
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t channel = 0; channel < 3; channel++) {
            uint8_t value = ((const uint8_t *)&colors[slot])[channel];
            for (uint8_t bit = 0; bit < 8; bit++) {
                if (value & 0x80) {
                    for (delay = 0; delay < 8; delay++) *port = high;
                } else {
                    for (delay = 0; delay < 2; delay++) *port = high;
                }
                for (delay = 0; delay < 6; delay++) *port = low;
                value <<= 1;
            }
        }
        /* Observed first-slot reset gap, not an assumed physical LED count. */
        if (!slot) for (delay = 0; delay < 10000; delay++) *port = low;
    }
    /* Latch the final frame even if the caller immediately has another update. */
    for (delay = 0; delay < 10000; delay++) *port = low;
    __asm volatile ("msr primask, %0" : : "r" (mask) : "memory");
}

static void fault(void)
{
    mico_rtos_lock_mutex(&a1_lock); a1_state.hardware_errors++; mico_rtos_unlock_mutex(&a1_lock);
}

static void fan_tick(const a1_config_t *c, uint8_t *speed)
{
    OSStatus err;
    if (!c->fan_on) {
        err = MicoGpioOutputHigh(MICO_GPIO_19);
        OSStatus stop = *speed ? MicoPwmStop(MICO_PWM_3) : kNoErr;
        *speed = 0;
        if (err == kNoErr) err = stop;
    } else if (*speed != c->speed) {
        int target = c->speed, next = *speed;
        if (next < target) next = target - next > 10 ? next + 10 : target;
        else next = next - target > 10 ? next - 10 : target;
        err = MicoPwmInitialize(MICO_PWM_3, 1000, 80.0f - 0.6f * next);
        if (err == kNoErr) err = MicoPwmStart(MICO_PWM_3);
        if (err == kNoErr) err = MicoGpioOutputLow(MICO_GPIO_19);
        if (err == kNoErr) *speed = next;
        else { MicoGpioOutputHigh(MICO_GPIO_19); MicoPwmStop(MICO_PWM_3); *speed = 0; }
    } else return;
    mico_rtos_lock_mutex(&a1_lock);
    a1_state.fan_ready = err == kNoErr; a1_state.applied_speed = *speed;
    if (err != kNoErr) a1_state.hardware_errors++;
    mico_rtos_unlock_mutex(&a1_lock);
}

static bool panel_init(void)
{
    MicoGpioInitialize(MICO_GPIO_7, OUTPUT_PUSH_PULL);
    MicoGpioInitialize(MICO_GPIO_8, OUTPUT_PUSH_PULL);
    return MicoI2cInitialize(&panel) == kNoErr && MicoI2cProbeDevice(&panel, 2);
}

static void panel_tick(uint32_t now, uint8_t *last_key, uint8_t *last_slider,
                       uint32_t *wifi_since, bool *wifi_sent)
{
    static key_gesture_t gestures[2];
    uint8_t reg = 0, raw[3] = {0}; mico_i2c_message_t message;
    OSStatus err = MicoI2cBuildCombinedMessage(&message, &reg, raw, 1, 3, 5);
    if (err == kNoErr) err = MicoI2cTransfer(&panel, &message, 1);
    a1_config_t cfg; a1_config_get(&cfg);
    mico_rtos_lock_mutex(&a1_lock);
    a1_state.panel_ready = err == kNoErr;
    a1_state.touch_active = err == kNoErr && raw[0] > 0 && raw[0] <= 99 &&
        !raw[1] && !raw[2] && !cfg.child_lock &&
        !a1_state.ota_active && !a1_state.reboot_pending;
    if (a1_state.touch_active) a1_state.led_until = now + 2000;
    if (err != kNoErr) a1_state.panel_errors++;
    else memcpy(a1_state.panel_raw, raw, sizeof(raw));
    mico_rtos_unlock_mutex(&a1_lock);
    if (err != kNoErr) { for(unsigned i=0;i<2;i++)key_gesture_sample(&gestures[i],false,false,now,i==1); *last_key = *last_slider = 0; *wifi_sent = false; return; }
    uint8_t key = raw[1] == 1 ? 1 : raw[2] == 1 ? 2 : 0;
    uint8_t slider = !key && raw[0] <= 99 ? raw[0] : 0;
    for(unsigned i=0;i<2;i++) {
        key_event_t event=key_gesture_sample(&gestures[i],true,key==i+1,now,i==1);
        if(event.type==KEY_EVENT_NETWORK_RESET || event.type==KEY_EVENT_FULL_RESET) a1_reset(event.type==KEY_EVENT_FULL_RESET);
        else if(event.type==KEY_EVENT_CLICKS) automation_button(i,event.count,0);
        else if(event.type==KEY_EVENT_HOLD) automation_button(i,0,event.count);
    }
    (void)wifi_since;
    *wifi_sent = false;
    if (slider && *last_slider && slider != *last_slider)
        a1_panel_event(false, (int)slider - *last_slider);
    *last_key = key; *last_slider = slider;
}

static void hardware_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    uint8_t speed = 0, key = 0, slider = 0;
    uint32_t fan_at = mico_rtos_get_time(), wifi_since = 0, probe_at = fan_at;
    bool wifi_sent = false, initialized = panel_init(), frame_sent = false;
    uint32_t previous[32] = {0};
    unsigned old_mode = INDICATOR_READY;
    uint32_t mode_since = fan_at;
    for (;;) {
        uint32_t now = mico_rtos_get_time();
        a1_config_t cfg; a1_state_t state;
        a1_config_get(&cfg); a1_state_get(&state);
        if ((uint32_t)(now - fan_at) >= 100) {
            bool on; unsigned desired;
            feature_effective_fan(cfg.fan_on, cfg.speed, &on, &desired);
            cfg.fan_on = on; cfg.speed = desired;
            fan_tick(&cfg, &speed); fan_at += 100;
            if ((uint32_t)(now - fan_at) >= 100) fan_at = now;
        }
        if (!initialized && (int32_t)(now - probe_at) >= 0) {
            initialized = panel_init(); probe_at = now + 3000;
            if (!initialized) {
                mico_rtos_lock_mutex(&a1_lock); a1_state.panel_errors++; mico_rtos_unlock_mutex(&a1_lock);
            }
        }
        if (initialized) panel_tick(now, &key, &slider, &wifi_since, &wifi_sent);
        /* Panel updates may have changed desired state and wake deadline. */
        a1_config_get(&cfg); a1_state_get(&state);
        bool visible = cfg.led_enabled && cfg.brightness &&
            (cfg.led_always_on || (int32_t)(state.led_until - now) > 0);
        uint32_t colors[32] = {0};
        uint32_t color = ((uint32_t)cfg.green << 16) | ((uint32_t)cfg.red << 8) | cfg.blue;
        if (visible) a1_ring_feedback(colors, state.applied_speed, cfg.brightness, color);
        feature_light_frame(colors, cfg.brightness, cfg.led_enabled,((uint32_t)cfg.green<<16)|((uint32_t)cfg.red<<8)|cfg.blue);
        /* Touch feedback shows the requested setting immediately, even while
         * PWM catches up or another effect is selected. Release restores it. */
        if (state.touch_active && visible)
            a1_ring_feedback(colors, cfg.speed, feature_light_cap(cfg.brightness), color);
        unsigned mode = indicator_mode(cfg.ssid[0] != 0, state.wifi_connected,
            cfg.mqtt_host[0] != 0, state.mqtt_connected && state.mqtt_status == MQTT_READY);
        if (mode != old_mode) mode_since = now;
        old_mode = mode;
        /* Overlay after business rendering. Night mode remains the final cap.
         * Slot 27 is the original Wi-Fi slot; physical centre needs DUT proof. */
        if (mode != INDICATOR_READY)
            colors[27] = indicator_on(mode, (uint32_t)(now - mode_since))
                ? (uint32_t)feature_light_cap(64) << 8 : 0;
        visible = false; for (unsigned i = 0; i < 32; i++) visible |= colors[i] != 0;
        if (!state.ota_active && (!frame_sent || memcmp(previous, colors, sizeof(colors)))) {
            ring_send(colors); memcpy(previous, colors, sizeof(colors)); frame_sent = true;
        }
        if (!state.ota_active) {
            mico_rtos_lock_mutex(&a1_lock); a1_state.led_visible = visible; mico_rtos_unlock_mutex(&a1_lock);
        }
        mico_thread_msleep(40);
    }
}

OSStatus a1_hardware_start(void)
{
    OSStatus err = MicoGpioOutputHigh(MICO_GPIO_19);
    if (err == kNoErr) err = MicoGpioInitialize(MICO_GPIO_19, OUTPUT_OPEN_DRAIN_NO_PULL);
    if (err == kNoErr) err = MicoGpioOutputHigh(MICO_GPIO_12);
    if (err == kNoErr) err = MicoGpioInitialize(MICO_GPIO_12, OUTPUT_PUSH_PULL);
    if (err == kNoErr) err = MicoGpioInitialize(MICO_GPIO_13, OUTPUT_PUSH_PULL);
    if (err == kNoErr) err = MicoGpioOutputLow(MICO_GPIO_13);
    if (err == kNoErr) err = MicoGpioInitialize(MICO_GPIO_14, INPUT_PULL_UP);
    if (err != kNoErr) { fault(); return err; }
    return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_hardware", hardware_worker, 4096, 0);
}
