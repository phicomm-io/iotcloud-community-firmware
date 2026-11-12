/* Real shared session/client and pinned Paho, with an in-memory transport. */
#define main existing_paho_scenarios
#include "paho.c"
#undef main

static unsigned cause_mode, cause_cycles, cause_closes;
static uint32_t cause_cycle_at;
static int cause_prepare(void *context, ph_mqtt_connection *spec) {
    (void)context;
    rx_at = rx_size = tx_size = 0;
    cause_cycles = cause_closes = 0;
    fail_send = missing_ack = subscribe_fail = false;
    open_fail = cause_mode == 4;
    *spec = (ph_mqtt_connection){&account, "broker", "user", "password",
        "device", "tx/availability", 1883, 50, NULL,"test","full"};
    return PH_MQTT_PREPARE_READY;
}
static int cause_initial(void *context) { (void)context; return MQTT_SUCCESS; }
static int cause_cycle(void *context) {
    (void)context;
    ++cause_cycles;
    cause_cycle_at = now_ms;
    if (cause_mode == 1) {
        fail_send = true;
        int rc = publish();
        assert(rc == MQTT_SOCKET_ERR);
        return rc;
    }
    if (cause_mode == 3)
        return MQTT_FAILURE; /* No evidence identifying a lower-level cause. */
    if (cause_mode == 5)
        return PH_MQTT_RECONFIGURE;
    return MQTT_SUCCESS;
}
static void cause_closing(void *context, bool connected, int rc) {
    (void)context;
    assert(connected == (cause_mode != 4));
    ++cause_closes;
    if (cause_mode == 6)
        fail_send = true; /* Graceful exit whose offline/close now fails. */
    if (cause_mode == 1) {
        assert(rc == MQTT_SOCKET_ERR);
        ph_mqtt_diagnostics d;
        ph_mqtt_snapshot(&d);
        assert(d.last_stage == PH_MQTT_ERROR_PUBLISH && d.last_error == MQTT_SOCKET_ERR);
    }
}
static bool cause_stop(void *context) {
    (void)context;
    return cause_closes || (cause_cycles &&
        (cause_mode != 2 || now_ms != cause_cycle_at));
}
static ph_mqtt_diagnostics run_cause(unsigned mode) {
    cause_mode = mode;
    cause_cycles = cause_closes = 0;
    ph_mqtt_session_hooks h = {NULL, cause_prepare, subscription_plan,
        cause_initial, cause_cycle, cause_closing, NULL, cause_stop};
    ph_mqtt_session_run(&client, &h);
    assert(cause_closes == 1 && !native_live && !charged_live);
    ph_mqtt_diagnostics d;
    ph_mqtt_snapshot(&d);
    assert(!d.protocol_depth && !d.bytes && !d.credit);
    return d;
}
static void unchanged_cause(ph_mqtt_diagnostics a, ph_mqtt_diagnostics b) {
    assert(a.last_stage == b.last_stage && a.last_error == b.last_error);
}
static int empty_cleanup(void) { return MQTT_SUCCESS; }
int main(void) {
    assert(mqtt_account_init(&account, 1, "a1", "test", "aabbccddeeff"));
    assert(ph_mqtt_owner_init(&account, 8192, &thread, accept));
    ph_mqtt_diagnostics failed = run_cause(1);
    /* Actual Paho send failure, generic exit and failed disconnect all count;
     * the first publication failure remains the displayed cause. */
    assert(failed.errors == 3 && failed.last_stage == PH_MQTT_ERROR_PUBLISH &&
           failed.last_error == MQTT_SOCKET_ERR);
    ph_mqtt_diagnostics normal = run_cause(2);
    unchanged_cause(failed, normal);
    assert(normal.errors == failed.errors);
    ph_mqtt_diagnostics unknown = run_cause(3);
    assert(unknown.last_stage == PH_MQTT_ERROR_SESSION && unknown.last_error == MQTT_FAILURE);
    assert(unknown.errors == normal.errors + 1);
    ph_mqtt_diagnostics network_failed = run_cause(4);
    assert(network_failed.last_stage == PH_MQTT_ERROR_NETWORK && network_failed.last_error == -1);
    assert(network_failed.errors == unknown.errors + 2);
    ph_mqtt_diagnostics reconfigured = run_cause(5);
    unchanged_cause(network_failed, reconfigured);
    assert(reconfigured.errors == network_failed.errors + 1);
    normal = run_cause(2);
    unchanged_cause(reconfigured, normal);
    assert(normal.errors == reconfigured.errors);

    ph_mqtt_diagnostics cleanup_failed = run_cause(6);
    assert(cleanup_failed.errors == normal.errors + 3);
    assert(cleanup_failed.last_stage == PH_MQTT_ERROR_PUBLISH &&
           cleanup_failed.last_error == MQTT_SOCKET_ERR);

    ph_mqtt_core *core = (ph_mqtt_core *)ph_mqtt_debug_core();
    core->errors = UINT32_MAX;
    failed = run_cause(1);
    assert(failed.errors == UINT32_MAX && failed.last_stage == PH_MQTT_ERROR_PUBLISH &&
           failed.last_error == MQTT_SOCKET_ERR);
    /* Wrong-thread begin cannot release the current session's latch. */
    current_thread = 2;
    ph_mqtt_session_begin();
    current_thread = 1;
    ph_mqtt_error(PH_MQTT_ERROR_INIT, -44);
    ph_mqtt_snapshot(&normal);
    unchanged_cause(failed, normal);
    network_failed = run_cause(4);
    assert(network_failed.errors == UINT32_MAX && network_failed.last_stage == PH_MQTT_ERROR_NETWORK);

    /* The previous-namespace cleanup is also a new Network session. */
    ph_mqtt_session_begin();
    ph_mqtt_error(PH_MQTT_ERROR_PUBLISH, -55);
    fail_send = false;
    rx_at = rx_size = tx_size = 0;
    assert(ph_mqtt_cleanup(&client, &account, "broker", 1883, "user", "password", empty_cleanup) == -1);
    ph_mqtt_snapshot(&normal);
    assert(normal.last_stage == PH_MQTT_ERROR_NETWORK && normal.last_error == -1);
    open_fail = false;
    rx_at = rx_size = tx_size = 0;
    assert(ph_mqtt_cleanup(&client, &account, "broker", 1883, "user", "password", empty_cleanup) == 0);
    ph_mqtt_snapshot(&normal);
    unchanged_cause(network_failed, normal);
    assert(!native_live && !charged_live);
    puts("actual Paho session first-cause: cleanup/persistence/unknown/network/reconfigure/saturation PASS");
}
