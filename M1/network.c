#include "m1.h"
#include "sntp.h"
#include <time.h>

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

static bool clock_sync(int timezone)
{
    struct hostent *host = gethostbyname("pool.ntp.org");
    if (!host || !host->h_addr_list[0]) return false;
    struct in_addr address;
    memcpy(&address.s_addr, host->h_addr_list[0], 4);
    ntp_timestamp_t stamp;
    if (sntp_get_time(&address, &stamp) != kNoErr || stamp.seconds < 1577836800u) return false;
    mico_utc_time_ms_t utc_ms = (uint64_t)stamp.seconds * 1000 + stamp.microseconds / 1000;
    mico_time_set_utc_time_ms(&utc_ms);
    time_t local = stamp.seconds + timezone * 60;
    struct tm t = *gmtime(&local);
    return m1_uart_clock(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

static void network_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    m1_config_t current, started;
    m1_config_get(&started);
    uint32_t lost_since = mico_rtos_get_time(), next_retry = lost_since;
    uint32_t next_clock = lost_since, scan_since = 0;
    bool ap = false, linked = false, clock_attempted = false;
    int clock_timezone = started.timezone_minutes;
    if (started.ssid[0]) station_start(&started);
    else ap = ap_start();
    for (;;) {
        uint32_t now = mico_rtos_get_time();
        m1_config_get(&current);
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
        if (ready) {
            if (ap) { micoWlanSuspendSoftAP(); ap = false; }
            if (!linked) { clock_attempted = false; next_clock = now; }
            linked = true; lost_since = now;
        } else {
            if (linked) { lost_since = now; linked = false; }
            if (!ap && (!current.ssid[0] || (uint32_t)(now - lost_since) >= 60000)) ap = ap_start();
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
        if (ready && !upgrading && (!clock_attempted || (int32_t)(now - next_clock) >= 0)) {
            bool synced = clock_sync(current.timezone_minutes);
            clock_attempted = true; next_clock = mico_rtos_get_time() + (synced ? 3600000u : 60000u);
        }
        mico_thread_msleep(500);
    }
}

OSStatus m1_network_start(void)
{
    OSStatus err = mico_system_notify_register(mico_notify_WIFI_SCAN_COMPLETED, (void *)scan_done, NULL);
    if (err != kNoErr) return err;
    return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "m1_network", network_worker, 6144, 0);
}
