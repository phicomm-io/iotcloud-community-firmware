#include "http_server/web_log.h"

#include "main.h"
#include "user_gpio.h"
#include <stdint.h>
#include "../common/gesture.h"
#include "../common/indicator.h"
#include "ota_server/signed_adapter.h"
#include "user_wifi.h"
#include "mqtt_server/user_mqtt_client.h"
#include "http_server/app_httpd.h"

mico_gpio_t relay[Relay_NUM] = {Relay_0, Relay_1, Relay_2, Relay_3, Relay_4, Relay_5};
char socket_status[32] = {0};
char btn_click_config[768] = {0};

static bool business_led;
static bool key_feedback_active, key_feedback_on;
void UserLedRefresh(void) {
    static unsigned old_mode = INDICATOR_READY;
    static uint32_t since;
    uint32_t now = mico_rtos_get_time();
    unsigned mode = indicator_mode(sys_config->micoSystemConfig.ssid[0] != 0,
        wifi_status == WIFI_STATE_CONNECTED, user_config->mqtt_host[0] != 0 && user_config->mqtt_port > 0,
        tc1_mqtt_status == MQTT_READY);
    if (mode != old_mode) since = now;
    old_mode = mode;
    bool on = mode == INDICATOR_READY ? business_led : indicator_on(mode, now - since);
    if (key_feedback_active) on = key_feedback_on;
    if (on) MicoGpioOutputHigh(Led); else MicoGpioOutputLow(Led);
}
void UserLedSet(char x) {
    business_led = x == -1 ? !business_led : x != 0;
    UserLedRefresh();
}

bool RelayOut(void) {
    int i;
    for (i = 0; i < SOCKET_NUM; i++) {
        if (user_config->socket_status[i] != 0) {
            return true;
        }
    }
    return false;
}

char* get_func_name(char func_code) {
    static char buffer[32];
    switch (func_code) {
        case SWITCH_ALL_SOCKETS:
            return "Toggle All Sockets";
        case SWITCH_SOCKET_1:
        case SWITCH_SOCKET_2:
        case SWITCH_SOCKET_3:
        case SWITCH_SOCKET_4:
        case SWITCH_SOCKET_5:
        case SWITCH_SOCKET_6:
            snprintf(buffer, sizeof(buffer), "Toggle Socket %d %.12s", func_code - 1,
                    user_config->socket_names[func_code - 1]);
            return buffer;
        case SWITCH_LED_ENABLE:
            return "Toggle LED";
        case REBOOT_SYSTEM:
            return "Reboot";
        case REBOOT_SOFT:
            return "Soft Reboot";
        case REBOOT_HTTP:
            return "REBOOT_HTTP";
        case CONFIG_WIFI:
            return "WiFi Config";
        case RESET_SYSTEM:
            return "Factory Reset";
        case SWITCH_CHILD_LOCK_ENABLE:
            return "Toggle ChildLick";
        case -1:
        case NO_FUNCTION:
            return "Unassigned";
        default:
            return "Unknown";
    }
}

/// 针对电源按钮的点击事件
/// \param index 判断短按（连击）时，代表连击次数，判断长按时代表长按秒数
/// \param short_func 功能码 在user_gpio.h中定义了
/// \param long_func 功能码 在user_gpio.h中定义了
void set_key_map(char user[],int index, char short_func, char long_func) {
    user[index] = ((long_func & 0x0F) << 4) | (short_func & 0x0F);
}

char get_short_func(char val) {
    char func = val & 0x0F;
    return (func == NO_FUNCTION) ? -1 : func;  // -1 表示未配置
}

char get_long_func(char val) {
    char func = (val >> 4) & 0x0F;
    return (func == NO_FUNCTION) ? -1 : func;  // -1 表示未配置
}


char *GetSocketStatus() {
    sprintf(socket_status, "%d,%d,%d,%d,%d,%d",
            user_config->socket_status[0],
            user_config->socket_status[1],
            user_config->socket_status[2],
            user_config->socket_status[3],
            user_config->socket_status[4],
            user_config->socket_status[5]);
    return socket_status;
}

char *GetButtonClickConfig() {
    char temp[32];
    int len = 0;
    int max_len =sizeof(btn_click_config);
    len += snprintf(btn_click_config + len, max_len - len, "[");

    for (int i = 1; i <= 30; i++) {
        char short_func = get_short_func(user_config->user[i]);
        char long_func  = get_long_func(user_config->user[i]);
//    key_log("WARNGIN:KEY func %d %d %d", i,short_func,long_func);

        snprintf(temp, sizeof(temp), "{\"%d\":[%d,%d]}%s", i, short_func, long_func, (i != 30) ? "," : "");
        int written = snprintf(btn_click_config + len, max_len - len, "%s", temp);
        if (written < 0 || written >= max_len-len-1) {
            btn_click_config[0]=0; return btn_click_config;
        }
        len += written;
    }
    snprintf(btn_click_config + len, max_len - len, "]");

    return btn_click_config;
}

void SetSocketStatus(char *socket_status) {
    int values[SOCKET_NUM];
    if (sscanf(socket_status, "%d,%d,%d,%d,%d,%d",
               &values[0], &values[1], &values[2], &values[3],
               &values[4], &values[5]) != SOCKET_NUM) return;
    for (int n=0;n<SOCKET_NUM;++n) if(values[n]!=0 && values[n]!=1) return;
    for (int n=0;n<SOCKET_NUM;++n) user_config->socket_status[n]=(char)values[n];
    int i = 0;
    for (i = 0; i < SOCKET_NUM; i++) {
        UserRelaySet(i, user_config->socket_status[i]);
        UserMqttSendSocketState(i);
    }
    UserMqttSendTotalSocketState();
    mico_system_context_update(sys_config);
}

/*UserRelaySet
 * 设置继电器开关
 *  i:编号 0-5
 * on:开关 0:关 1:开 -1:切换
 */
void UserRelaySet(unsigned char i, char on) {
    if (i < 0 || i >= SOCKET_NUM) return;

    if (on == Relay_ON) {
        MicoGpioOutputHigh(relay[i]);
    } else if (on == Relay_OFF) {
        MicoGpioOutputLow(relay[i]);
    } else if (on == Relay_TOGGLE) {
        if (user_config->socket_status[i] == Relay_OFF) {
            MicoGpioOutputHigh(relay[i]);
        } else {
            MicoGpioOutputLow(relay[i]);
        }
    }
    user_config->socket_status[i] = on >= 0 ? on : (user_config->socket_status[i] == 0 ? 1 : 0);

    if (RelayOut() && user_config->power_led_enabled) {
        UserLedSet(1);
    } else {
        UserLedSet(0);
    }
}

/*
 * 设置所有继电器开关
 * y: 0:全部关 1:全部开
 *
 */
void UserRelaySetAll(char y) {
    int i;
    for (i = 0; i < SOCKET_NUM; i++)
        UserRelaySet(i, y);
}

void UserSoftReboot(void) {
    /* MiCO application partitions are packaged images, not vector tables.
     * Restart through the platform so RTOS, IRQs and Wi-Fi are reinitialized. */
    MicoSystemReboot();
}

static void FixedRecovery(bool full) {
    if (!tc1_config_reserve()) return;
    user_config_t *old_user = malloc(sizeof(*old_user));
    if (!old_user) { tc1_config_release(); return; }
    *old_user = *user_config;
    __typeof__(sys_config->micoSystemConfig) old_system = sys_config->micoSystemConfig;
    OSStatus err = kNoErr;
    if (full) {
        err = mico_system_context_restore(sys_config);
    } else {
        sys_config->micoSystemConfig.ssid[0] = 0;
        sys_config->micoSystemConfig.user_key[0] = 0;
        sys_config->micoSystemConfig.user_keyLength = 0;
        sys_config->micoSystemConfig.key[0] = 0;
        sys_config->micoSystemConfig.keyLength = 0;
        user_config->web_password[0] = 0;
    }
    if (err == kNoErr) err = mico_system_context_update(sys_config);
    if (err != kNoErr) {
        *user_config = *old_user;
        sys_config->micoSystemConfig = old_system;
        free(old_user); tc1_config_release(); return;
    }
    free(old_user);
    MicoSystemReboot();
}

static void KeyEventHandler(int num, boolean longPress) {
    key_log("WARNGIN:Power key %s %d %s", !longPress ? "quick clicked" : "longPressed", num,
            num > 1 ? (longPress ? "seconds" : "times") : (longPress ? "second" : "time"));
    if (childLockEnabled) return;
    if (num > 30 || num <= 0)
        return;
    int function = !longPress ? get_short_func(user_config->user[num]) : get_long_func(
            user_config->user[num]);
            boolean showLog= childLockEnabled==0;
    switch (function) {
        case SWITCH_ALL_SOCKETS:
         if (childLockEnabled)
                break;
            if (RelayOut()) {
                UserRelaySetAll(0);
            } else {
                UserRelaySetAll(1);
            }
            mico_system_context_update(sys_config);
            for (int i = 0; i < SOCKET_NUM; i++) {
                UserMqttSendSocketState(i);
            }
            UserMqttSendTotalSocketState();
            break;
        case SWITCH_SOCKET_1:
        case SWITCH_SOCKET_2:
        case SWITCH_SOCKET_3:
        case SWITCH_SOCKET_4:
        case SWITCH_SOCKET_5:
        case SWITCH_SOCKET_6:
        if (childLockEnabled)
                        break;
            UserRelaySet(function - 1, Relay_TOGGLE);
            UserMqttSendSocketState(function - 1);
            UserMqttSendTotalSocketState();
            mico_system_context_update(sys_config);
            break;
        case SWITCH_LED_ENABLE:
        if (childLockEnabled)
                        break;
            MQTT_LED_ENABLED = MQTT_LED_ENABLED == 0 ? 1 : 0;
            if (RelayOut() && MQTT_LED_ENABLED) {
                UserLedSet(1);
            } else {
                UserLedSet(0);
            }
            UserMqttSendLedState();
            mico_system_context_update(sys_config);
            break;
        case SWITCH_CHILD_LOCK_ENABLE:
            showLog=true;
            user_config->user[0] = user_config->user[0] == 0 ? 1 : 0;
            childLockEnabled = user_config->user[0];
            mico_system_context_update(sys_config);
            UserMqttSendChildLockState();
            break;
        case REBOOT_SYSTEM:
        if (childLockEnabled)
                        break;
            MicoSystemReboot();
            break;
        case REBOOT_SOFT:
        if (childLockEnabled)
                        break;
            UserSoftReboot();
            break;
        case REBOOT_HTTP:
        if (childLockEnabled)
                        break;
            AppHttpdStop();
            mico_rtos_thread_sleep(1);
            AppHttpdStart();
            break;
        case CONFIG_WIFI:
        if (childLockEnabled)
                        break;
            StartLedBlink(3);
            UserMqttDeInit();
            micoWlanSuspendStation();
            ApInit(true);
            break;
        case RESET_SYSTEM:
        if (childLockEnabled)
                        break;
            StartLedBlink(8);
            mico_system_context_restore(sys_config);
            mico_rtos_thread_sleep(1);
            MicoSystemReboot();
            break;
        default:
            break;
    }
    key_log("WARNGIN:%s",showLog? get_func_name(function):"child lock enabled,ignore key event !");
}

mico_timer_t user_key_timer;
// 全局静态变量声明
static uint8_t click_count = 0;
static mico_timer_t click_end_timer;
uint16_t key_time = 0;

static mico_timer_t led_blink_timer;
static bool timer_initialized = false;

static uint8_t total_blinks = 0;
static uint8_t blink_counter = 0;
static bool led_state = false;
#define BUTTON_LONG_PRESS_TIME    10     //100ms*10=1s
// 定时器回调
static void _led_blink_timer_handler(void *arg)
{
    if (blink_counter >= total_blinks) {
        UserLedSet(0);  // 闪烁完成，灭灯
        mico_stop_timer(&led_blink_timer);
        mico_deinit_timer(&led_blink_timer);
        timer_initialized = false;
        return;
    }

    led_state = !led_state;
    UserLedSet(led_state ? 1 : 0);
    blink_counter++;
}

// 安全重入的启动函数
void StartLedBlink(uint8_t times)
{
    if (times == 0) return;

    // 如果之前已启动，先停止并清理
    if (timer_initialized) {
        mico_stop_timer(&led_blink_timer);
        mico_deinit_timer(&led_blink_timer);
        timer_initialized = false;
    }

    total_blinks = times * 2;
    blink_counter = 0;
    led_state = false;

    mico_init_timer(&led_blink_timer, 100, _led_blink_timer_handler, NULL);
    mico_start_timer(&led_blink_timer);
    timer_initialized = true;
}
static void ClickEndTimeoutHandler(void *arg) {
    if (click_count <= 0) {
        click_count = 0;
        return;
    }
    KeyEventHandler(click_count,false);
    click_count = 0;
}

static key_gesture_t key_gesture;
static void KeyTimeoutHandler(void *arg) {
    (void)arg;
    uint32_t now=mico_rtos_get_time();
    key_event_t event=key_gesture_sample(&key_gesture,true,!MicoGpioInputGet(Button),now,true);
    key_feedback_active=key_gesture.stable && key_gesture.candidate;
    key_feedback_on=key_hold_feedback(&key_gesture,now);
    UserLedRefresh();
    if(event.type==KEY_EVENT_NETWORK_RESET || event.type==KEY_EVENT_FULL_RESET) FixedRecovery(event.type==KEY_EVENT_FULL_RESET);
    else if(event.type==KEY_EVENT_CLICKS) automation_button(0,event.count,0);
    else if(event.type==KEY_EVENT_HOLD) automation_button(0,0,event.count);
}
void KeyInit(void) {
    MicoGpioInitialize(Button, INPUT_PULL_UP);
    mico_rtos_init_timer(&user_key_timer, 40, KeyTimeoutHandler, NULL);
    mico_rtos_start_timer(&user_key_timer);
}
