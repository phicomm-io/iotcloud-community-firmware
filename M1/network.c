#include "../common/watchdog.h"
#include "m1.h"
#include "sntp.h"
#include <time.h>

#define CLOCK_SYNC_INTERVAL_MS (60u * 60u * 1000u)
#define CLOCK_RETRY_INTERVAL_MS (60u * 1000u)

static const char *const clock_servers[] = {
    "ntp1.aliyun.com",
    "time1.cloud.tencent.com",
    "cn.ntp.org.cn",
    "pool.ntp.org",
    "cn.pool.ntp.org"
};

static void scan_done(ScanResult *list, void *arg)
{
    UNUSED_PARAMETER(arg);
    mico_rtos_lock_mutex(&m1_lock);
    m1_state.scan_count = 0;
    for (unsigned i = 0; list && i < (unsigned char)list->ApNum; i++) {
        char ssid[33]; memcpy(ssid, list->ApList[i].ssid, 32); ssid[32] = 0;
        if (!ssid[0]) continue;
        unsigned j;
        for (j = 0; j < m1_state.scan_count; j++)
            if (!strcmp(ssid, m1_state.scan[j].ssid)) break;
        if (j == m1_state.scan_count) {
            if (j == 32) continue;
            strcpy(m1_state.scan[j].ssid, ssid);
            m1_state.scan[j].rssi = list->ApList[i].rssi;
            m1_state.scan_count++;
        } else if (list->ApList[i].rssi > m1_state.scan[j].rssi)
            m1_state.scan[j].rssi = list->ApList[i].rssi;
    }
    m1_state.scan_busy = false;
    mico_rtos_unlock_mutex(&m1_lock);
}

bool m1_scan_start(void)
{
    mico_rtos_lock_mutex(&m1_lock);
    if (m1_state.scan_busy) { mico_rtos_unlock_mutex(&m1_lock); return false; }
    m1_state.scan_busy = true;
    mico_rtos_unlock_mutex(&m1_lock);
    micoWlanStartScan(); /* SDK 3.6.4 exposes this as a void notification API. */
    return true;
}

static bool station_start(const m1_config_t *c)
{
    if (!c->ssid[0]) return false;
    network_InitTypeDef_st net = {0};
    memcpy(net.wifi_ssid, c->ssid, sizeof(net.wifi_ssid));
    memcpy(net.wifi_key, c->wifi_password, sizeof(net.wifi_key));
    net.wifi_mode = Station; net.dhcpMode = DHCP_Client; net.wifi_retry_interval = 1000;
    return micoWlanStart(&net) == kNoErr;
}

static bool ap_start(void)
{
    network_InitTypeDef_st net = {0};
    strcpy(net.wifi_ssid, m1_state.ap_name);
    net.wifi_mode = Soft_AP; net.dhcpMode = DHCP_Server;
    strcpy(net.local_ip_addr, "192.168.0.1"); strcpy(net.gateway_ip_addr, "192.168.0.1");
    strcpy(net.net_mask, "255.255.255.0"); strcpy(net.dnsServer_ip_addr, "192.168.0.1");
    return micoWlanStart(&net) == kNoErr;
}

#define WIFI_PREFIX m1
#define WIFI_CONFIG m1_config_t
#define WIFI_STATE m1_state
#define WIFI_LOCK m1_lock
#define WIFI_GET m1_config_get
#define WIFI_SAVE m1_config_save
#include "../common/wifi_trial.inc"

static bool clock_sync(int timezone, const char *preferred)
{
    /* Try every source before scheduling a retry; never replace valid time
     * with a failed or implausible NTP response. */
    mico_rtos_lock_mutex(&m1_lock);
    strcpy(m1_state.ntp_status, "syncing");
    mico_rtos_unlock_mutex(&m1_lock);
    for (unsigned i = 0; i <= sizeof(clock_servers) / sizeof(clock_servers[0]); i++) {
        const char *server = i ? clock_servers[i - 1] : preferred;
        if (!server[0] || (i && !strcmp(server, preferred))) continue;
        mico_rtos_lock_mutex(&m1_lock);
        snprintf(m1_state.ntp_server, sizeof(m1_state.ntp_server), "%s", server);
        mico_rtos_unlock_mutex(&m1_lock);
        struct hostent *host = gethostbyname(server);
        if (!host || !host->h_addr_list || !host->h_addr_list[0]) {
            custom_log("M1 NTP", "%s: DNS failed", server);
            continue;
        }
        struct in_addr address;
        memcpy(&address.s_addr, host->h_addr_list[0], 4);
        ntp_timestamp_t stamp;
        if (sntp_get_time(&address, &stamp) != kNoErr || stamp.seconds < 1577836800u) {
            custom_log("M1 NTP", "%s: request failed or invalid time", server);
            continue;
        }
        mico_utc_time_ms_t utc_ms = (uint64_t)stamp.seconds * 1000 + stamp.microseconds / 1000;
        mico_time_set_utc_time_ms(&utc_ms);
        feature_clock_valid();
        time_t local = stamp.seconds + timezone * 60;
        struct tm t = *gmtime(&local);
        bool queued = m1_uart_clock(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.ntp_synced = true;
        m1_state.ntp_screen_queued = queued;
        m1_state.ntp_last_success = stamp.seconds;
        strcpy(m1_state.ntp_status, queued ? "synced" : "screen_queue_failed");
        mico_rtos_unlock_mutex(&m1_lock);
        custom_log("M1 NTP", "%s: UTC %lu, screen queued %d", server, (unsigned long)stamp.seconds, queued);
        return queued;
    }
    custom_log("M1 NTP", "All sources failed; retry in 60 seconds");
    mico_rtos_lock_mutex(&m1_lock);
    strcpy(m1_state.ntp_status, "failed");
    mico_rtos_unlock_mutex(&m1_lock);
    return false;
}

static void network_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    m1_config_t current, started;
    m1_config_get(&started);
    uint32_t lost_since = mico_rtos_get_time(), next_retry = lost_since;
    uint32_t next_clock = lost_since, scan_since = 0;
    bool ever_connected = false;
    bool ap = false, linked = false, clock_attempted = false;
    bool mqtt_started = false, mqtt_retry_pending = false;
    uint32_t mqtt_retry_at = 0;
    int clock_timezone = started.timezone_minutes;
    char clock_host[64]; m1_ntp_host_get(clock_host);
    if (started.ssid[0]) station_start(&started);
    else ap = ap_start();
    for (;;) {
        uint32_t now = mico_rtos_get_time();
        m1_config_get(&current);
        bool trial_started = wifi_trial_prepare(&current, now);
        if (trial_started) {
            micoWlanSuspendStation();
            started = current;
            station_start(&started);
            next_retry = now + 30000u;
        }

        char preferred[64]; m1_ntp_host_get(preferred);
        if (strcmp(clock_host, preferred)) {
            strcpy(clock_host, preferred); clock_attempted = false;
        }
        if (strcmp(current.ssid, started.ssid) || strcmp(current.wifi_password, started.wifi_password)) {
            micoWlanSuspendStation();
            started = current; linked = false; lost_since = now; next_retry = now;
            station_start(&started);
        }
        if (current.timezone_minutes != clock_timezone) {
            clock_timezone = current.timezone_minutes; clock_attempted = false;
        }
        LinkStatusTypeDef link = {0}; IPStatusTypedef ip = {0};
        bool ready = micoWlanGetLinkStatus(&link) == kNoErr && link.is_connected &&
            micoWlanGetIPStatus(&ip, Station) == kNoErr && ip.ip[0] && strcmp(ip.ip, "0.0.0.0");
        /* DHCP must belong to the requested SSID, never the previous AP. */
        ready = ready && !strncmp((const char *)link.ssid, current.ssid, sizeof(link.ssid));
        int trial_result = wifi_trial_finish(ready, now);
        if (trial_result < 0) {
            micoWlanSuspendStation();
            m1_config_get(&current);
            started = current;
            station_start(&current);
            ready = false;
            next_retry = now + 30000u;
            if (!current.ssid[0] && !ap) ap = ap_start();
        }
        if (ready) {
            if (ap) { micoWlanSuspendSoftAP(); ap = false; }
            if (!linked) { clock_attempted = false; next_clock = now; }
            ever_connected = true;
            linked = true; lost_since = now;
        } else {
            if (linked) { lost_since = now; linked = false; }
            if (!ap && (!current.ssid[0] || (!ever_connected && (uint32_t)(now - lost_since) >= 300000u))) ap = ap_start();
            if (current.ssid[0] && (int32_t)(now - next_retry) >= 0 && !link.is_connected) {
                station_start(&current); next_retry = now + 30000;
            }
        }
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.wifi_connected = ready; m1_state.ap_active = ap;
        snprintf(m1_state.ip, sizeof(m1_state.ip), "%s", ready ? ip.ip : "");
        bool scanning = m1_state.scan_busy;
        if (scanning && !scan_since) scan_since = now;
        if (scanning && (uint32_t)(now - scan_since) > 15000) m1_state.scan_busy = false;
        if (!scanning) scan_since = 0;
        bool upgrading = m1_state.ota_active || m1_state.reboot_pending;
        bool reboot = m1_state.reboot_pending && (int32_t)(now - m1_state.reboot_at) >= 0;
        mico_rtos_unlock_mutex(&m1_lock);
        if (reboot) MicoSystemReboot();
        watchdog_beat(WATCHDOG_NETWORK);
        /* Do not reserve the 16 KiB MQTT stack while first-time provisioning
         * needs the heap for SoftAP, HTTP and Wi-Fi credential validation.
         * This worker is the sole creator; reconnects stay with MQTT's owner. */
        if (ready && !upgrading && !mqtt_started &&
            (!mqtt_retry_pending || (int32_t)(now - mqtt_retry_at) >= 0)) {
            OSStatus err = m1_mqtt_start();
            if (err == kNoErr) mqtt_started = true;
            else {
                mqtt_retry_pending = true;
                mqtt_retry_at = mico_rtos_get_time() + 30000u;
                custom_log("M1", "MQTT start deferred: %ld", (long)err);
            }
        }
        if (ready && !upgrading && (!clock_attempted || (int32_t)(now - next_clock) >= 0) &&
            watchdog_network_begin()) {
            bool synced = clock_sync(current.timezone_minutes, clock_host);
            watchdog_network_end();
            clock_attempted = true;
            /* Use monotonic ticks, so correcting wall time cannot move the deadline. */
            next_clock = mico_rtos_get_time() + (synced ? CLOCK_SYNC_INTERVAL_MS : CLOCK_RETRY_INTERVAL_MS);
            mico_rtos_lock_mutex(&m1_lock);
            m1_state.ntp_next_at = next_clock;
            mico_rtos_unlock_mutex(&m1_lock);
        }
        mico_thread_msleep(500);
    }
}

OSStatus m1_network_start(void)
{
    OSStatus err = mico_system_notify_register(mico_notify_WIFI_SCAN_COMPLETED, (void *)scan_done, NULL);
    if (err != kNoErr) return err;
    err = mico_system_notify_register(mico_notify_DHCP_COMPLETED, (void *)wifi_trial_dhcp, NULL);
    if (err != kNoErr) return err;
    return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "m1_network", network_worker, 6144, 0);
}
