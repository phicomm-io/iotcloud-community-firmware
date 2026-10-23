#include "../common/watchdog.h"
#include "a1.h"
#include "fresh_air.h"

static void scan_done(ScanResult *list, void *arg)
{
    UNUSED_PARAMETER(arg);
    mico_rtos_lock_mutex(&a1_lock);
    a1_state.scan_count = 0;
    for (unsigned i = 0; list && i < (unsigned char)list->ApNum; i++) {
        char ssid[33]; memcpy(ssid, list->ApList[i].ssid, 32); ssid[32] = 0;
        if (!ssid[0]) continue;
        unsigned j;
        for (j = 0; j < a1_state.scan_count; j++)
            if (!strcmp(ssid, a1_state.scan[j].ssid)) break;
        if (j == a1_state.scan_count) {
            if (j == 32) continue;
            strcpy(a1_state.scan[j].ssid, ssid);
            a1_state.scan[j].rssi = list->ApList[i].rssi;
            a1_state.scan_count++;
        } else if (list->ApList[i].rssi > a1_state.scan[j].rssi)
            a1_state.scan[j].rssi = list->ApList[i].rssi;
    }
    a1_state.scan_busy = false;
    mico_rtos_unlock_mutex(&a1_lock);
}

bool a1_scan_start(void)
{
    mico_rtos_lock_mutex(&a1_lock);
    if (a1_state.scan_busy) { mico_rtos_unlock_mutex(&a1_lock); return false; }
    a1_state.scan_busy = true;
    mico_rtos_unlock_mutex(&a1_lock);
    micoWlanStartScan(); /* SDK 3.6.4 exposes this as a void notification API. */
    return true;
}

static bool station_start(const a1_config_t *c)
{
    if (!c->ssid[0]) return false;
    network_InitTypeDef_st net = {0};
    memcpy(net.wifi_ssid, c->ssid, sizeof(net.wifi_ssid));
    memcpy(net.wifi_key, c->wifi_password, sizeof(net.wifi_key));
    net.wifi_mode = Station; net.dhcpMode = DHCP_Client; net.wifi_retry_interval = 10000;
    OSStatus err = micoWlanStart(&net);
    custom_log("A1", "Station start: %ld", (long)err);
    return err == kNoErr;
}

static bool ap_start(void)
{
    network_InitTypeDef_st net = {0};
    strcpy(net.wifi_ssid, a1_state.ap_name);
    net.wifi_mode = Soft_AP; net.dhcpMode = DHCP_Server;
    strcpy(net.local_ip_addr, "192.168.0.1"); strcpy(net.gateway_ip_addr, "192.168.0.1");
    strcpy(net.net_mask, "255.255.255.0"); strcpy(net.dnsServer_ip_addr, "192.168.0.1");
    OSStatus err = micoWlanStart(&net);
    custom_log("A1", "AP %s start: %ld", net.wifi_ssid, (long)err);
    return err == kNoErr;
}

#define WIFI_PREFIX a1
#define WIFI_CONFIG a1_config_t
#define WIFI_STATE a1_state
#define WIFI_LOCK a1_lock
#define WIFI_GET a1_config_get
#define WIFI_SAVE a1_config_save
#include "../common/wifi_trial.inc"

static void network_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    a1_config_t current, started;
    a1_config_get(&started);
    uint32_t lost_since = mico_rtos_get_time(), next_retry = lost_since;
    uint32_t scan_since = 0;
    bool ever_connected = false;
    bool ap = false, linked = false, station_requested = false;
    bool mqtt_started = false, mqtt_retry_pending = false;
    uint32_t mqtt_retry_at = 0;
    if (started.ssid[0]) {
        station_requested = station_start(&started);
        next_retry = mico_rtos_get_time() + 30000;
    }
    else ap = ap_start();
    for (;;) {
        uint32_t now = mico_rtos_get_time();
        a1_config_get(&current);
        bool trial_started = wifi_trial_prepare(&current, now);
        if (trial_started) {
            micoWlanSuspendStation();
            started = current;
            station_requested = station_start(&started);
            next_retry = now + 30000u;
        }

        if (strcmp(current.ssid, started.ssid) || strcmp(current.wifi_password, started.wifi_password)) {
            micoWlanSuspendStation();
            started = current; linked = false; lost_since = now;
            station_requested = station_start(&started);
            next_retry = mico_rtos_get_time() + 30000;
        }
        LinkStatusTypeDef link = {0}; IPStatusTypedef ip = {0};
        bool ready = micoWlanGetLinkStatus(&link) == kNoErr && link.is_connected &&
            micoWlanGetIPStatus(&ip, Station) == kNoErr && ip.ip[0] && strcmp(ip.ip, "0.0.0.0");
        /* DHCP must belong to the requested SSID, never the previous AP. */
        ready = ready && !strncmp((const char *)link.ssid, current.ssid, sizeof(link.ssid));
        int trial_result = wifi_trial_finish(ready, now);
        if (trial_result < 0) {
            micoWlanSuspendStation();
            a1_config_get(&current);
            started = current;
            station_requested = station_start(&current);
            ready = false;
            next_retry = now + 30000u;
            if (!current.ssid[0] && !ap) ap = ap_start();
        }
        /* Do not put the scan results and OTA strings on the network stack. */
        mico_rtos_lock_mutex(&a1_lock);
        bool ap_requested = a1_state.ap_requested;
        bool ota_active = a1_state.ota_active;
        bool reboot_pending = a1_state.reboot_pending;
        mico_rtos_unlock_mutex(&a1_lock);
        if (ap_requested && !ap) ap = ap_start();
        if (ready) {
            if (ap && !ap_requested) { micoWlanSuspendSoftAP(); ap = false; }
            ever_connected = true;
            linked = true; lost_since = now;
        } else {
            if (linked) { lost_since = now; linked = false; }
            if (!ap && (!current.ssid[0] || (!ever_connected && (uint32_t)(now - lost_since) >= 300000u))) ap = ap_start();
            /* An accepted request reconnects in the SDK. Reissuing it while
             * disconnected starts overlapping application/SDK retry cycles.
             * Only retry here if the initial API request was rejected. */
            if (current.ssid[0] && !station_requested && (int32_t)(now - next_retry) >= 0) {
                station_requested = station_start(&current);
                next_retry = mico_rtos_get_time() + 30000;
            }
        }
        mico_rtos_lock_mutex(&a1_lock);
        a1_state.wifi_connected = ready; a1_state.ap_active = ap;
        snprintf(a1_state.ip, sizeof(a1_state.ip), "%s", ready ? ip.ip : "");
        bool scanning = a1_state.scan_busy;
        if (scanning && !scan_since) scan_since = now;
        if (scanning && (uint32_t)(now - scan_since) > 15000) a1_state.scan_busy = false;
        if (!scanning) scan_since = 0;
        bool reboot = a1_state.reboot_pending && (int32_t)(now - a1_state.reboot_at) >= 0;
        mico_rtos_unlock_mutex(&a1_lock);
        if (reboot) {
#ifdef A1_FRESH_AIR
            fresh_shutdown();
#endif
            MicoGpioOutputHigh(MICO_GPIO_19);
            MicoPwmStop(MICO_PWM_3);
            MicoSystemReboot();
        }
        a1_config_flush(false);
        watchdog_beat(WATCHDOG_NETWORK);
        /* Do not reserve the 16 KiB MQTT stack while first-time provisioning
         * needs the heap for SoftAP, HTTP and Wi-Fi credential validation.
         * This worker is the sole creator; reconnects stay with MQTT's owner. */
        if (ready && !ota_active && !reboot_pending && !mqtt_started &&
            (!mqtt_retry_pending || (int32_t)(now - mqtt_retry_at) >= 0)) {
            OSStatus err = a1_mqtt_start();
            if (err == kNoErr) mqtt_started = true;
            else {
                mqtt_retry_pending = true;
                mqtt_retry_at = mico_rtos_get_time() + 30000u;
                custom_log("A1", "MQTT start deferred: %ld", (long)err);
            }
        }
        if (ready && !ota_active && !reboot_pending && watchdog_network_begin()) {
            feature_clock_tick();
            watchdog_network_end();
        }
        mico_thread_msleep(500);
    }
}

OSStatus a1_network_start(void)
{
    OSStatus err = mico_system_notify_register(mico_notify_WIFI_SCAN_COMPLETED, (void *)scan_done, NULL);
    if (err != kNoErr) return err;
    err = mico_system_notify_register(mico_notify_DHCP_COMPLETED, (void *)wifi_trial_dhcp, NULL);
    if (err != kNoErr) return err;
    /* micoWlanStart and clock/config work also consume this thread's stack. */
    return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_network", network_worker, 8192, 0);
}
