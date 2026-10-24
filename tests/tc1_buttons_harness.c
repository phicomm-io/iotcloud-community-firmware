/* Production gesture, automation, control, GPIO and JSON memory policy with
 * host substitutes for the RTOS, Flash and physical pins. */
#define AUTOMATION_TC1
#define PHICOMM_DEVICE_OTA 1
#define MOC 1
#define __MAIN_H_
#define __USER_KEY_H_
#include "mico.h"
#include "common/automation.h"
#include "common/gesture.h"
#include "common/indicator.h"
#include "common/mqtt_account.h"
#include "TC1/status_guard.h"
#include "TC1/ota_server/signed_adapter.h"
#include "common/ota/mode.h"
/* Exercise real command admission with an injected OTA state, while keeping
 * the unrelated download worker out of this native control fixture. */
#undef PHICOMM_DEVICE_OTA

#define SOCKET_NUM 6
#define Relay_ON 1
#define Relay_OFF 0
#define Relay_TOGGLE -1
#define Button 23
#define Led 5
#define WIFI_STATE_CONNECTED 1
typedef struct { int free_memory; } micoMemInfo_t;
static micoMemInfo_t memory = {65536};
static micoMemInfo_t *MicoGetMemoryInfo(void) { return &memory; }
#include "common/service.c"

uint32_t test_now;
static struct {
    automation_config_t automation;
    char socket_status[SOCKET_NUM], user[32], mqtt_host[128];
    int power_led_enabled, mqtt_port;
    unsigned unrelated;
} config;
#define user_config (&config)
static struct { struct { char ssid[33]; } micoSystemConfig; } system_config;
#define sys_config (&system_config)
static int childLockEnabled;
static unsigned tc1_mqtt_status, ha_refresh_pending;
static int wifi_status = WIFI_STATE_CONNECTED;
static bool state_locked, reserved, ota_active, fail_save, fail_thread;
static unsigned saves, allocation_attempts, product_calls, reboots, recoveries;
static bool deny_allocations, pressed, gpio_state[32];
static unsigned char relay[SOCKET_NUM] = {6, 8, 10, 7, 9, 18};
static bool business_led, key_feedback_active, key_feedback_on;
static key_gesture_t key_gesture;

void tc1_status_lock(void) { assert(!state_locked); state_locked = true; }
void tc1_status_unlock(void) { assert(state_locked); state_locked = false; }
bool tc1_status_begin(void) {
    tc1_status_lock();
    if (reserved) { tc1_status_unlock(); return false; }
    return true;
}
bool tc1_config_reserve(void) {
    assert(!state_locked);
    if (reserved || ota_active) return false;
    reserved = true;
    return true;
}
void tc1_config_release(void) { assert(reserved); reserved = false; }
void ota_job_init(void) {}
bool ota_job_active(void) { return ota_active; }
bool ota_job_cancel(void) { return false; }
json_object *ota_job_status(void) { return NULL; }
void automation_load(automation_config_t *out) { *out = config.automation; }
bool automation_save(const automation_config_t *next) { config.automation = *next; return true; }
json_object *service_product_capabilities(void) { return NULL; }
json_object *service_product(const char *action, json_object *p, const char **code) {
    (void)action; (void)p;
    ++product_calls;
    *code = "busy";
    return NULL;
}
#include "tests/stubs/status_wire.inc"

static void *button_malloc(size_t size) {
    ++allocation_attempts;
    return deny_allocations ? NULL : malloc(size);
}
#define malloc button_malloc
#include "common/automation.c"
static OSStatus mico_system_context_update(void *context) {
    assert(context == sys_config && reserved && !state_locked);
    ++saves;
    return fail_save ? kUnknownErr : kNoErr;
}
static int create_reboot_thread(void *thread, int priority, const char *name,
                               void (*entry)(mico_thread_arg_t), unsigned stack, int arg) {
    (void)thread; (void)priority; (void)name; (void)entry; (void)stack; (void)arg;
    assert(reserved && !state_locked);
    if (fail_thread) return kUnknownErr;
    ++reboots;
    return kNoErr;
}
#define mico_rtos_create_thread create_reboot_thread
#define mico_rtos_thread_msleep mico_thread_msleep
#define MicoSystemReboot() ((void)0)
#define mico_rtos_delete_thread(thread) ((void)(thread))
void UserRelaySet(unsigned char i, char on);
void UserRelaySetAll(char on);
void UserLedSet(char on);
void UserLedRefresh(void);
bool RelayOut(void);
#include "TC1/control.c"
#undef malloc

static void MicoGpioOutputHigh(unsigned pin) { assert(!state_locked); gpio_state[pin] = true; }
static void MicoGpioOutputLow(unsigned pin) { assert(!state_locked); gpio_state[pin] = false; }
static int MicoGpioInputGet(unsigned pin) { assert(pin == Button); return !pressed; }
static void FixedRecovery(bool full) { (void)full; ++recoveries; }
/* Replaced by the test runner with the actual TC1 functions. */
#include "tc1_button_functions.inc"

static void sample(bool down, unsigned ms) {
    pressed = down;
    for (unsigned n = 0; n < ms; n += 40) {
        test_now += 40;
        KeyTimeoutHandler(NULL);
    }
}
static void tap(void) { sample(true, 200); sample(false, 600); }
static void hold(unsigned ms) { sample(true, ms); sample(false, 600); }
static void sockets(bool on) {
    for (unsigned i = 0; i < SOCKET_NUM; ++i)
        assert(config.socket_status[i] == on && gpio_state[relay[i]] == on);
}
static void mapping(unsigned slot, unsigned kind, unsigned target, unsigned value) {
    config.automation.buttons[0][slot] = (automation_action_t){
        .kind = kind, .target = target, .value = value
    };
}
static void gestures(void) {
    mapping(0, AUTO_OUTLET, 255, 2);
    mapping(30, AUTO_INDICATOR, 0, 2);
    mapping(31, AUTO_CHILD_LOCK, 0, 2);
    for (unsigned state = MQTT_DISCONNECTED; state <= MQTT_READY; ++state) {
        tc1_mqtt_status = state;
        for (unsigned low = 0; low < 2; ++low) {
            memory.free_memory = low ? 16000 : 65536;
            deny_allocations = low;
            assert(json_heap_permit(1) == !low);
            unsigned prior = saves;
            ha_refresh_pending = 0;
            tap(); sockets(true);
            assert(ha_refresh_pending == 1);
            tap(); sockets(false);
            hold(1200); assert(config.power_led_enabled == 1);
            hold(2200); assert(childLockEnabled == 1 && config.user[0] == 1);
            tap(); sockets(false);
            hold(1200); assert(config.power_led_enabled == 1);
            hold(2200); assert(childLockEnabled == 0 && config.user[0] == 0);
            hold(1200); assert(config.power_led_enabled == 0);
            assert(saves == prior + 6 && !reserved);
            printf("PASS TC1 gestures: mqtt=%s free=%d\n", mqtt_status_name(state), memory.free_memory);
        }
    }
}
static void control_failures(void) {
    /* Continue with all button-path malloc calls rejected and JSON below its
     * reserve. The GPIO, persisted config and asynchronous refresh must agree. */
    unsigned prior = saves;
    reserved = true;
    tap(); sockets(false); assert(saves == prior && reserved);
    reserved = false;
    ota_active = true;
    tap(); sockets(false); assert(saves == prior && !reserved);
    ota_active = false;
    fail_save = true;
    ha_refresh_pending = 0;
    tap(); sockets(false);
    hold(1200); assert(config.power_led_enabled == 0);
    hold(2200); assert(childLockEnabled == 0 && config.user[0] == 0);
    assert(saves == prior + 3 && !reserved && ha_refresh_pending == 0);
    fail_save = false;
    tap(); sockets(true); tap(); sockets(false);

    /* Multi-click and the last configurable hold slot use the same native
     * route; a long press must not additionally trigger the short mapping. */
    mapping(1, AUTO_OUTLET, 1, 1);
    sample(true, 200); sample(false, 200);
    sample(true, 200); sample(false, 600);
    for (unsigned i = 0; i < SOCKET_NUM; ++i) assert(config.socket_status[i] == (i == 1));
    mapping(33, AUTO_OUTLET, 1, 0);
    hold(4200); sockets(false);

    /* Single-outlet toggles and a mixed all-outlet toggle retain their
     * previous semantics. Schedules can run while the physical key is locked. */
    mapping(0, AUTO_OUTLET, 2, 2);
    tap();
    for (unsigned i = 0; i < SOCKET_NUM; ++i) assert(config.socket_status[i] == (i == 2));
    mapping(0, AUTO_OUTLET, 255, 2);
    tap(); sockets(false);
    childLockEnabled = config.user[0] = 1;
    automation_action_t scheduled = {.kind = AUTO_OUTLET, .target = 5, .value = 1};
    assert(auto_execute(&scheduled, false));
    assert(config.socket_status[5] == 1 && gpio_state[relay[5]]);
    hold(2200); assert(!childLockEnabled);
    tap(); sockets(false);

    prior = saves;
    automation_action_t invalid = {.kind = AUTO_OUTLET, .target = 6, .value = 1};
    assert(!tc1_automation_execute(&invalid, true));
    invalid.target = 0; invalid.value = 3;
    assert(!tc1_automation_execute(&invalid, true));
    invalid.kind = AUTO_FAN; invalid.value = 1;
    assert(!tc1_automation_execute(&invalid, true));
    assert(!tc1_automation_execute(NULL, true) && saves == prior);
    automation_button(AUTOMATION_KEYS, 1, 0);
    automation_button(0, 31, 0);
    automation_button(0, 0, 5);
    assert(saves == prior);

    /* Thread creation failure must release admission so the next press works. */
    mapping(0, AUTO_REBOOT, 0, 0);
    fail_thread = true;
    tap(); assert(reboots == 0 && !reserved);
    fail_thread = false;
    tap(); assert(reboots == 1 && reserved);
    tc1_config_release();
    mapping(0, AUTO_OUTLET, 255, 1);
    tap(); sockets(true);
    mapping(0, AUTO_OUTLET, 255, 0);
    tap(); sockets(false);
    sample(true, 5200); sample(false, 600); assert(recoveries == 1);
    sample(true, 10200); sample(false, 600); assert(recoveries == 2);
    assert(!state_locked && !reserved && !product_calls && !allocation_attempts);
    assert(config.unrelated == 0x1234abcd);
    puts("PASS TC1 admission, persistence rollback, schedules, reboot and fixed recovery");
}
int main(void) {
    service_init();
    strcpy(system_config.micoSystemConfig.ssid, "test-wifi");
    strcpy(config.mqtt_host, "unreachable.invalid");
    config.mqtt_port = 1883;
    config.unrelated = 0x1234abcd;
    automation_defaults(&config.automation);
    gestures();
    control_failures();
    return 0;
}
