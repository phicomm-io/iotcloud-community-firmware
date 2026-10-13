#include "a1.h"

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
    net.wifi_mode = Station; net.dhcpMode = DHCP_Client; net.wifi_retry_interval = 1000;
    return micoWlanStart(&net) == kNoErr;
}

static bool ap_start(void)
{
    network_InitTypeDef_st net = {0};
    strcpy(net.wifi_ssid, a1_state.ap_name);
    net.wifi_mode = Soft_AP; net.dhcpMode = DHCP_Server;
    strcpy(net.local_ip_addr, "192.168.0.1"); strcpy(net.gateway_ip_addr, "192.168.0.1");
    strcpy(net.net_mask, "255.255.255.0"); strcpy(net.dnsServer_ip_addr, "192.168.0.1");
    return micoWlanStart(&net) == kNoErr;
}

static void network_worker(mico_thread_arg_t arg)
{
    UNUSED_PARAMETER(arg);
    a1_config_t current, started;
    a1_config_get(&started);
    uint32_t lost_since = mico_rtos_get_time(), next_retry = lost_since;
    uint32_t scan_since = 0;
    bool ap = false, linked = false;
    if (started.ssid[0]) station_start(&started);
    else ap = ap_start();
    for (;;) {
        uint32_t now = mico_rtos_get_time();
        a1_config_get(&current);
        if (strcmp(current.ssid, started.ssid) || strcmp(current.wifi_password, started.wifi_password)) {
            micoWlanSuspendStation();
            started = current; linked = false; lost_since = now; next_retry = now;
            station_start(&started);
        }
        LinkStatusTypeDef link = {0}; IPStatusTypedef ip = {0};
        bool ready = micoWlanGetLinkStatus(&link) == kNoErr && link.is_connected &&
            micoWlanGetIPStatus(&ip, Station) == kNoErr && ip.ip[0] && strcmp(ip.ip, "0.0.0.0");
        a1_state_t snapshot; a1_state_get(&snapshot);
        if (snapshot.ap_requested && !ap) ap = ap_start();
        if (ready) {
            if (ap && !snapshot.ap_requested) { micoWlanSuspendSoftAP(); ap = false; }
            linked = true; lost_since = now;
        } else {
            if (linked) { lost_since = now; linked = false; }
            if (!ap && (!current.ssid[0] || (uint32_t)(now - lost_since) >= 60000)) ap = ap_start();
            if (current.ssid[0] && (int32_t)(now - next_retry) >= 0 && !link.is_connected) {
                station_start(&current); next_retry = now + 30000;
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
        if (reboot) MicoSystemReboot();
        a1_config_flush(false);
        mico_thread_msleep(500);
    }
}

OSStatus a1_network_start(void)
{
    OSStatus err = mico_system_notify_register(mico_notify_WIFI_SCAN_COMPLETED, (void *)scan_done, NULL);
    if (err != kNoErr) return err;
    return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_network", network_worker, 4096, 0);
}
