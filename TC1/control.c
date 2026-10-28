#include "main.h"
#include "../common/service_mp.h"
#include "user_gpio.h"
#include "status_guard.h"
#include "mqtt_server/user_mqtt_client.h"
#include "ota_server/signed_adapter.h"

bool tc1_automation_load_button(unsigned key, unsigned slot, automation_action_t *out)
{
    if (!tc1_status_begin()) return false;
    *out = user_config->automation.buttons[key][slot];
    tc1_status_unlock();
    return true;
}

static void tc1_control_reboot(mico_thread_arg_t arg)
{
    (void)arg;
    mico_rtos_thread_msleep(1500);
    MicoSystemReboot();
    mico_rtos_delete_thread(NULL);
}

/* Internal controls share command/OTA admission with HTTP and MQTT, but need
 * neither a status JSON graph nor a backup of the entire saved configuration. */
bool tc1_automation_execute(const automation_action_t *action, bool button)
{
    if (!action) return false;
    unsigned kind = action->kind;
    if (kind == AUTO_NONE) return true;
    if (kind != AUTO_OUTLET && kind != AUTO_INDICATOR &&
        kind != AUTO_CHILD_LOCK && kind != AUTO_REBOOT) return false;
    if (kind != AUTO_REBOOT && action->value > 2) return false;
    if (kind == AUTO_OUTLET && action->target != 255 && action->target >= SOCKET_NUM) return false;
    if (!service_control_begin()) return false;

    bool ok = false;
    if (!tc1_config_reserve()) goto done;
    tc1_status_lock();
    bool locked = childLockEnabled != 0;
    tc1_status_unlock();
    if (button && locked && kind != AUTO_CHILD_LOCK) {
        service_event("info", "button", "child_locked");
        tc1_config_release();
        goto done;
    }
    if (kind == AUTO_REBOOT) {
        ok = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "reboot",
                                    tc1_control_reboot, 1024, 0) == kNoErr;
        /* Keep the reservation until reboot, as for a transport command. */
        if (!ok) tc1_config_release();
        goto done;
    }

    char old_sockets[SOCKET_NUM], old_child;
    int old_led, old_lock;
    tc1_status_lock();
    memcpy(old_sockets, user_config->socket_status, sizeof(old_sockets));
    old_led = user_config->power_led_enabled;
    old_child = user_config->user[0];
    old_lock = childLockEnabled;
    tc1_status_unlock();

    bool enabled = action->value != 0;
    if (kind == AUTO_OUTLET) {
        if (action->value == 2) {
            bool any_on = false;
            for (unsigned i = 0; i < SOCKET_NUM; ++i)
                if (action->target == 255 || action->target == i) any_on |= old_sockets[i] != 0;
            enabled = !any_on;
        }
        if (action->target == 255) UserRelaySetAll(enabled);
        else UserRelaySet(action->target, enabled);
    } else if (kind == AUTO_INDICATOR) {
        if (action->value == 2) enabled = !old_led;
        TC1_STATUS_WRITE(user_config->power_led_enabled = enabled);
        UserLedSet(RelayOut() && enabled);
    } else {
        if (action->value == 2) enabled = !old_lock;
        TC1_STATUS_WRITE(user_config->user[0] = enabled; childLockEnabled = enabled);
    }

    ok = mico_system_context_update(sys_config) == kNoErr;
    if (!ok) {
        TC1_STATUS_WRITE(user_config->power_led_enabled = old_led;
                         user_config->user[0] = old_child; childLockEnabled = old_lock);
        if (kind == AUTO_OUTLET)
            for (unsigned i = 0; i < SOCKET_NUM; ++i) UserRelaySet(i, old_sockets[i]);
        UserLedSet(RelayOut() && old_led);
        service_event("error", "command", "storage_error");
    } else UserMqttRefreshState(false);
    tc1_config_release();
done:
    service_control_end();
    service_event(ok ? "info" : "warn", "command", ok ? "accepted" : "rejected");
    return ok;
}
