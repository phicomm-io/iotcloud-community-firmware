#include "../common/automation.h"
#include "a1.h"
#include "light.h"
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
/* Only the hardware worker accesses this transient overlay. */
static a1_key_light_t key_light;

/* Cycle-timed candidate for DUT validation. The SDK enables DWT CYCCNT
 * during startup; use it without resetting the shared counter. Keep data and
 * instructions in SRAM and preserve PRIMASK across the complete frame.
 * Physical order is G,R,B; shared packed colors remain 0xGGRRBB.
 * Nominal at 125 MHz: zero high 320 ns, one high 800 ns, bit >=1.25 us.
 * Exact pin waveform and LED part identity still require measurement.
 */
__attribute__((section(".fast"), noinline, optimize("O2")))
static void ring_send(const uint32_t *colors)
{
    uint8_t bytes[A1_RING_POSITIONS * 3];
    for (unsigned i = 0; i < A1_RING_POSITIONS; i++) {
        bytes[i * 3] = colors[i] >> 16;
        bytes[i * 3 + 1] = colors[i] >> 8;
        bytes[i * 3 + 2] = colors[i];
    }
    volatile uint32_t *cycles = (volatile uint32_t *)0xe0001004u;
    volatile uint32_t *port = (volatile uint32_t *)0x40001000u;
    uint32_t mask;
    __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (mask) : : "memory");
    uint32_t other = *port, high = other | 0x1000u, low = other & ~0x1000u;
    *port = low;
    uint32_t start = *cycles;
    /* Start/end reset spans protect against a previously interrupted frame. */
    while ((uint32_t)(*cycles - start) < 37500u) {}
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        unsigned value = bytes[i];
        for (unsigned bit = 0; bit < 8; bit++, value <<= 1) {
            unsigned high_cycles = value & 0x80 ? 100 : 40;
            *port = high;
            start = *cycles;
            while ((uint32_t)(*cycles - start) < high_cycles) {}
            *port = low;
            while ((uint32_t)(*cycles - start) < 156u) {}
        }
    }
    start = *cycles;
    while ((uint32_t)(*cycles - start) < 37500u) {}
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
    static bool attempted;
    if (attempted) MicoI2cFinalize(&panel);
    attempted = true;
    /* Release SDA/SCL rather than driving either bus line high. Clock a
     * slave out of an interrupted byte, then issue STOP before reinitializing. */
    MicoGpioInitialize(MICO_GPIO_7, OUTPUT_OPEN_DRAIN_PULL_UP);
    MicoGpioInitialize(MICO_GPIO_8, OUTPUT_OPEN_DRAIN_PULL_UP);
    MicoGpioOutputHigh(MICO_GPIO_8);
    MicoGpioOutputHigh(MICO_GPIO_7);
    mico_thread_msleep(1);
    for (unsigned i = 0; i < 9 && !MicoGpioInputGet(MICO_GPIO_8); i++) {
        MicoGpioOutputLow(MICO_GPIO_7); mico_thread_msleep(1);
        MicoGpioOutputHigh(MICO_GPIO_7); mico_thread_msleep(1);
        if (!MicoGpioInputGet(MICO_GPIO_7)) break;
    }
    MicoGpioOutputLow(MICO_GPIO_7);
    MicoGpioOutputLow(MICO_GPIO_8);
    mico_thread_msleep(1);
    MicoGpioOutputHigh(MICO_GPIO_7); mico_thread_msleep(1);
    MicoGpioOutputHigh(MICO_GPIO_8); mico_thread_msleep(1);
    /* The A1 panel can reject the address-only probe while accepting its
     * register read. Let panel_tick's real transfer determine readiness. */
    OSStatus err = MicoI2cInitialize(&panel);
    mico_rtos_lock_mutex(&a1_lock); a1_state.panel_last_error = err; mico_rtos_unlock_mutex(&a1_lock);
    return err == kNoErr;
}

static bool panel_tick(uint32_t now, uint8_t *last_key, uint8_t *last_slider,
                       uint32_t *wifi_since, bool *wifi_sent)
{
    static key_gesture_t gestures[2];
    static uint32_t feedback_second[2];
    uint8_t reg = 0, raw[3] = {0}; mico_i2c_message_t message;
    OSStatus err = MicoI2cBuildCombinedMessage(&message, &reg, raw, 1, 3, 5);
    if (err == kNoErr) err = MicoI2cTransfer(&panel, &message, 1);
    mico_rtos_lock_mutex(&a1_lock);
    a1_state.panel_ready = err == kNoErr;
    a1_state.panel_last_error = err;
    if (err != kNoErr) a1_state.panel_errors++;
    else memcpy(a1_state.panel_raw, raw, sizeof(raw));
    mico_rtos_unlock_mutex(&a1_lock);
    if (err != kNoErr) { for(unsigned i=0;i<2;i++)key_gesture_sample(&gestures[i],false,false,now,i==1); key_light.active = false; *last_key = 0; *last_slider = 255; *wifi_sent = false; return false; }
    uint8_t key = raw[1] == 1 ? 1 : raw[2] == 1 ? 2 : 0;
    uint8_t slider = !key && raw[0] <= 99 ? raw[0] : 255;
    for(unsigned i=0;i<2;i++) {
        bool was_down = gestures[i].stable;
        key_event_t event=key_gesture_sample(&gestures[i],true,key==i+1,now,i==1);
        bool pressed = !was_down && gestures[i].stable && !gestures[i].blocked;
        uint32_t held = now - gestures[i].pressed_at;
        /* Start one short pulse per held second, not on every polling tick. */
        if (pressed) feedback_second[i] = 0;
        bool hold_pulse = gestures[i].stable && gestures[i].candidate &&
            !gestures[i].blocked && held / 1000u > feedback_second[i];
        if (hold_pulse) feedback_second[i] = held / 1000u;
        if (pressed || hold_pulse) {
            mico_rtos_lock_mutex(&a1_config_lock);
            bool locked = a1_config->child_lock;
            mico_rtos_unlock_mutex(&a1_config_lock);
            a1_key_light_start(&key_light, i, locked, !pressed, now);
            mico_rtos_lock_mutex(&a1_lock);
            a1_state.fan_feedback_until = 0;
            mico_rtos_unlock_mutex(&a1_lock);
        }
        if(event.type==KEY_EVENT_NETWORK_RESET || event.type==KEY_EVENT_FULL_RESET) a1_reset(event.type==KEY_EVENT_FULL_RESET);
        else if(event.type==KEY_EVENT_CLICKS) automation_button(i,event.count,0);
        else if(event.type==KEY_EVENT_HOLD) automation_button(i,0,event.count);
    }
    (void)wifi_since;
    *wifi_sent = false;
    if (slider != 255 && *last_slider != 255 && slider != *last_slider)
        a1_panel_event(false, (int)slider - *last_slider);
    *last_key = key; *last_slider = slider;
    return true;
}

static void hardware_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    uint8_t speed = 0, key = 0, slider = 255;
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
        if (initialized && !panel_tick(now, &key, &slider, &wifi_since, &wifi_sent)) {
            initialized = false; probe_at = mico_rtos_get_time() + 3000;
        }
        /* Panel updates may have changed desired state and wake deadline. */
        a1_config_get(&cfg); a1_state_get(&state);
        bool visible = cfg.led_enabled && cfg.brightness &&
            (cfg.led_always_on || (int32_t)(state.led_until - now) > 0);
        uint32_t colors[32] = {0};
        if (visible) {
            uint32_t r = cfg.red * cfg.brightness / 255, g = cfg.green * cfg.brightness / 255;
            uint32_t b = cfg.blue * cfg.brightness / 255;
            uint32_t color = (g << 16) | (r << 8) | b;
            a1_fan_frame(colors, cfg.fan_on ? cfg.speed : 0, color);
        }
        feature_light_frame(colors, cfg.brightness, cfg.led_enabled,((uint32_t)cfg.green<<16)|((uint32_t)cfg.red<<8)|cfg.blue);
        unsigned mode = indicator_mode(cfg.ssid[0] != 0, state.wifi_connected,
            cfg.mqtt_host[0] != 0, state.mqtt_connected && state.mqtt_status == MQTT_READY);
        if (mode != old_mode) mode_since = now;
        old_mode = mode;
        /* Overlay after business rendering. Night mode remains the final cap.
         * Slot 27 is the original Wi-Fi slot; physical centre needs DUT proof. */
        bool feedback = (int32_t)(state.fan_feedback_until - now) > 0;
        if (mode != INDICATOR_READY && !feedback)
            colors[27] = indicator_on(mode, (uint32_t)(now - mode_since))
                ? (uint32_t)feature_light_cap(64) << 8 : 0;
        if (feedback) {
            unsigned brightness = cfg.led_enabled ? feature_light_cap(cfg.brightness) : 0;
            uint32_t color = ((uint32_t)(cfg.green * brightness / 255) << 16) |
                ((uint32_t)(cfg.red * brightness / 255) << 8) | (cfg.blue * brightness / 255);
            a1_fan_frame(colors, cfg.fan_on ? cfg.speed : 0, color);
        }
        /* Interaction feedback can briefly wake a disabled business light.
         * Night mode still caps it; configured LED state stays untouched. */
        a1_key_light_frame(&key_light, now,
            feature_light_cap(cfg.brightness ? cfg.brightness : 64), colors);
        /* Diagnostic frame overrides every overlay; never saved to Flash. */
        a1_test_frame(colors, state.light_test_slot, now, state.light_test_until,
            state.light_test_neighbors);
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
    /* Button automation nests service/status/config handlers below this loop.
     * The loop and status frames alone exceed 4 KiB on the pinned compiler. */
    return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_hardware", hardware_worker, A1_HARDWARE_STACK_SIZE, 0);
}
