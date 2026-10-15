#include "user_wifi.h"

#include "main.h"
#include "http_server/json_buffer.h"
#include "mico_socket.h"
#include "user_gpio.h"
#include "http_server/web_log.h"
#include "mqtt_server/user_mqtt_client.h"

char wifi_status = WIFI_STATE_NOCONNECT;

mico_timer_t wifi_led_timer;
IpStatus ip_status = { 0, ZZ_AP_LOCAL_IP, ZZ_AP_LOCAL_IP, ZZ_AP_NET_MASK };
static volatile bool wifi_reconnect_thread_running = false;
static volatile bool wifi_config_ap_active = false;
static volatile bool wifi_saved_ssid_seen = false;
static volatile bool wifi_boot_scan_pending = false;

#define WIFI_BOOT_SCAN_TIMEOUT_SECONDS 300
#define WIFI_RECONNECT_INTERVAL_SECONDS 10

static void WifiStartReconnect(bool allow_boot_ap);

static bool WifiHasSavedConfig(void)
{
    return sys_config && sys_config->micoSystemConfig.ssid[0] != 0;
}

static OSStatus WifiStartStationFromSavedConfig(void)
{
    network_InitTypeDef_st wNetConfig;
    OSStatus err;

    memset(&wNetConfig, 0, sizeof(network_InitTypeDef_st));
    wNetConfig.wifi_mode = Station;
    snprintf(wNetConfig.wifi_ssid, sizeof(wNetConfig.wifi_ssid), "%s",
             sys_config->micoSystemConfig.ssid);
    snprintf((char*)wNetConfig.wifi_key, sizeof(wNetConfig.wifi_key), "%s",
             sys_config->micoSystemConfig.user_key);
    wNetConfig.dhcpMode = DHCP_Client;
    wNetConfig.wifi_retry_interval = 6000;
    err = micoWlanStart(&wNetConfig);
    if (err != kNoErr)
    {
        wifi_log("Start station failed[%d]", err);
    }
    return err;
}

//wifi宸茶繛鎺ヨ幏鍙栧埌IP鍦板潃鍥炶皟
static void WifiGetIpCallback(IPStatusTypedef *pnet, void * arg)
{
    strcpy(ip_status.ip, pnet->ip);
    strcpy(ip_status.gateway, pnet->gate);
    strcpy(ip_status.mask, pnet->mask);

    wifi_log("got IP:%s", pnet->ip);
    wifi_status = WIFI_STATE_CONNECTED;
    wifi_config_ap_active = false;
    wifi_saved_ssid_seen = true;
    //UserFunctionCmdReceived(1,"{\"cmd\":\"device report\"}");
}

//wifi杩炴帴鐘舵€佹敼鍙樺洖璋?
static void WifiStatusCallback(WiFiEvent status, void* arg)
{
    if (status == NOTIFY_STATION_UP) //wifi杩炴帴鎴愬姛
    {
        //user_config->last_wifi_status = status;
        sys_config->micoSystemConfig.reserved = status;
        mico_system_context_update(sys_config);

        OSStatus status = micoWlanSuspendSoftAP(); //鍏抽棴AP
        if (status != kNoErr)
        {
            wifi_log("close ap error[%d]", status);
        }

        ip_status.mode = 1;
        wifi_config_ap_active = false;
        wifi_saved_ssid_seen = true;
        //wifi_status = WIFI_STATE_CONNECTED;
    }
    else if (status == NOTIFY_STATION_DOWN) //wifi鏂紑
    {
        //涓嶄繚瀛楴OTIFY_STATION_DOWN鍒癴lash锛岀‘淇濋噸鍚悗浠嶄細灏濊瘯杩炴帴WiFi
        //sys_config->micoSystemConfig.reserved = status;
        //mico_system_context_update(sys_config);

        wifi_status = WIFI_STATE_NOCONNECT;
        if (!mico_rtos_is_timer_running(&wifi_led_timer))
        {
            mico_rtos_start_timer(&wifi_led_timer);
        }

        //鍚姩WiFi鑷姩閲嶈繛绾跨▼
        if (!wifi_config_ap_active)
        {
            WifiStartReconnect(false);
        }
    }
    else if (status == NOTIFY_AP_UP)
    {
        ip_status.mode = 0;
    }
}

#define WIFI_SCAN_MAX 24
static mico_mutex_t scan_lock;
static struct {char ssid[33];int rssi,security;} scan_results[WIFI_SCAN_MAX];
static int scan_count;
static bool scan_busy;
static mico_time_t scan_started;
OSStatus WifiScanStart(void) {
    mico_rtos_lock_mutex(&scan_lock);
    if(scan_busy){mico_rtos_unlock_mutex(&scan_lock);return kNoErr;}
    scan_busy=true;scan_count=0;mico_time_get_time(&scan_started);mico_rtos_unlock_mutex(&scan_lock);
    micoWlanStartScanAdv();return kNoErr;
}
char *WifiScanJson(void) {
    mico_rtos_lock_mutex(&scan_lock);
    mico_time_t now;mico_time_get_time(&now);
    if(scan_busy && (mico_time_t)(now-scan_started)>=20000)scan_busy=false;
    size_t capacity=64+(size_t)scan_count*256;
    char *data=malloc(capacity);if(!data){mico_rtos_unlock_mutex(&scan_lock);return NULL;}
    JsonBuffer b={data,0,capacity,true};
    jb_printf(&b,"{\"scanning\":%s,\"networks\":[",scan_busy?"true":"false");
    for(int i=0;i<scan_count;++i){jb_printf(&b,"%s{",i?",":"");jb_field(&b,"ssid",scan_results[i].ssid);jb_printf(&b,"\"rssi\":%d,\"security\":%d}",scan_results[i].rssi,scan_results[i].security);}
    jb_printf(&b,"]}");mico_rtos_unlock_mutex(&scan_lock);
    if(!b.valid){free(data);return NULL;}return data;
}
void WifiScanCallback(ScanResult_adv *result,void *arg) {
    if(!result)return;
    bool boot_scan=wifi_boot_scan_pending;wifi_boot_scan_pending=false;
    for(int i=0;i<result->ApNum;++i)if(WifiHasSavedConfig() && !strncmp(result->ApList[i].ssid,sys_config->micoSystemConfig.ssid,sizeof(result->ApList[i].ssid)))wifi_saved_ssid_seen=true;
    mico_rtos_lock_mutex(&scan_lock);
    if(boot_scan && !scan_busy){mico_rtos_unlock_mutex(&scan_lock);return;}
    scan_count=0;
    for(int i=0;i<result->ApNum && scan_count<WIFI_SCAN_MAX;++i){
        if(!result->ApList[i].ssid[0])continue;
        memcpy(scan_results[scan_count].ssid,result->ApList[i].ssid,32);scan_results[scan_count].ssid[32]=0;
        scan_results[scan_count].rssi=result->ApList[i].rssi;
        scan_results[scan_count++].security=result->ApList[i].security;
    }
    scan_busy=false;mico_rtos_unlock_mutex(&scan_lock);
}

//100ms瀹氭椂鍣ㄥ洖璋?
static void WifiLedTimerCallback(void* arg)
{
    static unsigned int num = 0;
    num++;

    switch (wifi_status)
    {
        case WIFI_STATE_FAIL:
            wifi_log("wifi connect fail");
            UserLedSet(0);
            mico_rtos_stop_timer(&wifi_led_timer);
            break;
        case WIFI_STATE_NOCONNECT:
            //wifi_connect_sys_config();
            break;
        case WIFI_STATE_CONNECTING:
            num = 0;
            UserLedSet(-1);
            break;
        case WIFI_STATE_CONNECTED:
            if (!(MQTT_SERVER[0] < 0x20 || MQTT_SERVER[0] > 0x7f || MQTT_SERVER_PORT < 1)){
                UserMqttInit();
            }
            UserLedSet(0);
            mico_rtos_stop_timer(&wifi_led_timer);
            if (RelayOut()&&user_config->power_led_enabled)
                UserLedSet(1);
            else
                UserLedSet(0);
            break;
    }
}

static void WifiReconnectThread(mico_thread_arg_t arg)
{
    LinkStatusTypeDef link_status = { 0 };
    bool allow_boot_ap = (bool)arg;
    int elapsed = 0;

    if (!WifiHasSavedConfig())
    {
        wifi_log("WiFi reconnect: no saved SSID, stop retry");
        goto exit;
    }

    micoWlanGetLinkStatus(&link_status);
    if (link_status.is_connected == 1)
    {
        wifi_status = WIFI_STATE_CONNECTED;
        goto exit;
    }

    wifi_status = WIFI_STATE_CONNECTING;
    wifi_log("WiFi reconnect started");

    /* Restart Station once; micoWlanStart keeps retrying in the background. */
    micoWlanSuspendStation();
    mico_thread_msleep(100);
    WifiStartStationFromSavedConfig();

    while (1)
    {
        if (wifi_status == WIFI_STATE_CONNECTED) break;
        if (wifi_config_ap_active) break;

        if (!WifiHasSavedConfig())
        {
            wifi_log("WiFi reconnect: no saved SSID, stop retry");
            break;
        }

        if (allow_boot_ap && !wifi_saved_ssid_seen && !wifi_boot_scan_pending)
        {
            wifi_boot_scan_pending = true;
            micoWlanStartScanAdv();
        }

        mico_thread_sleep(WIFI_RECONNECT_INTERVAL_SECONDS);
        elapsed += WIFI_RECONNECT_INTERVAL_SECONDS;

        if (allow_boot_ap && !wifi_saved_ssid_seen
            && elapsed >= WIFI_BOOT_SCAN_TIMEOUT_SECONDS)
        {
            wifi_log("Saved WiFi not found during boot, switching to configuration AP");
            ApInit(true);
            break;
        }
    }

exit:
    wifi_log("WiFi reconnect thread exit, status=%d", wifi_status);
    mico_rtos_suspend_all_thread();
    wifi_reconnect_thread_running = false;
    mico_rtos_resume_all_thread();
    mico_rtos_delete_thread(NULL);
}

static void WifiStartReconnect(bool allow_boot_ap)
{
    if (!WifiHasSavedConfig())
    {
        wifi_log("WiFi reconnect skipped: no saved SSID");
        return;
    }
    mico_rtos_suspend_all_thread();
    if (wifi_reconnect_thread_running)
    {
        mico_rtos_resume_all_thread();
        wifi_log("WiFi reconnect thread already running");
        return;
    }
    wifi_reconnect_thread_running = true;
    mico_rtos_resume_all_thread();

    wifi_log("Starting WiFi reconnect thread...");
    if (mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "wifi_reconn",
                                (mico_thread_function_t)WifiReconnectThread,
                                0x800, (mico_thread_arg_t)allow_boot_ap) != kNoErr)
    {
        mico_rtos_suspend_all_thread();
        wifi_reconnect_thread_running = false;
        mico_rtos_resume_all_thread();
        wifi_log("WiFi reconnect thread create failed");
    }
}

void WifiConnect(char* wifi_ssid, char* wifi_key)
{
    wifi_log("WifiConnect wifi_ssid[%s] wifi_key[******]", wifi_ssid);
    //wifi閰嶇疆鍒濆鍖?
    //淇濆瓨wifi鍙婂瘑鐮佸埌Flash
    snprintf(sys_config->micoSystemConfig.ssid, sizeof(sys_config->micoSystemConfig.ssid), "%s", wifi_ssid);
    snprintf(sys_config->micoSystemConfig.user_key, sizeof(sys_config->micoSystemConfig.user_key), "%s", wifi_key);
    sys_config->micoSystemConfig.user_keyLength = strlen(wifi_key);
    mico_system_context_update(sys_config);
    wifi_config_ap_active = true;
    wifi_status = WIFI_STATE_NOCONNECT;
    micoWlanSuspend();
    mico_thread_msleep(100);
    WifiStartStationFromSavedConfig();
    wifi_config_ap_active = false;
}

void WifiInit(void)
{
    mico_rtos_init_mutex(&scan_lock);
    //wifi鐘舵€佷笅led闂儊瀹氭椂鍣ㄥ垵濮嬪寲
    mico_rtos_init_timer(&wifi_led_timer, 100, (void*)WifiLedTimerCallback, NULL);
    //wifi宸茶繛鎺ヨ幏鍙栧埌IP鍦板潃 鍥炶皟
    mico_system_notify_register(mico_notify_DHCP_COMPLETED, (void*)WifiGetIpCallback, NULL);
    //wifi杩炴帴鐘舵€佹敼鍙樺洖璋?
    mico_system_notify_register(mico_notify_WIFI_STATUS_CHANGED, (void*)WifiStatusCallback, NULL);
    //wifi鎵弿缁撴灉鍥炶皟
    mico_system_notify_register(mico_notify_WIFI_SCAN_ADV_COMPLETED, (void*)WifiScanCallback, NULL);

    //sntp_init();
    //鍚姩瀹氭椂鍣ㄥ紑濮嬭繘琛寃ifi杩炴帴
    if (!mico_rtos_is_timer_running(&wifi_led_timer)) mico_rtos_start_timer(&wifi_led_timer);
}

void WifiStartOnBoot(void)
{
    LinkStatusTypeDef LinkStatus = { 0 };

    micoWlanGetLinkStatus(&LinkStatus);
    if (LinkStatus.is_connected == 1)
    {
        wifi_status = WIFI_STATE_CONNECTED;
        wifi_config_ap_active = false;
        ip_status.mode = 1;
        return;
    }

    if (WifiHasSavedConfig())
    {
        wifi_saved_ssid_seen = false;
        WifiStartReconnect(true);
    }
    else
    {
        ApInit(true);
    }
}

void ApConfig(char* name, char* key)
{
    strncpy(user_config->ap_name, name, 32);
    strncpy(user_config->ap_key, key, 32);
    user_config->ap_name[31] = 0;
    user_config->ap_key[31] = 0;
    wifi_log("ApConfig ap_name[%s] ap_key[******]", user_config->ap_name);
    ApInit(false);
    mico_system_context_update(sys_config);
}

void ApInit(bool use_defaul)
{
    OSStatus err;

    if (use_defaul)
    {
        sprintf(user_config->ap_name, ZZ_AP_NAME, str_mac + 6);
        sprintf(user_config->ap_key, "%s", ZZ_AP_KEY);
        wifi_log("ApInit use_defaul[true] key[]");
    }

    wifi_config_ap_active = true;
    wifi_status = WIFI_STATE_NOCONNECT;

    /* Stop background station retries before creating the DHCP server. */
    micoWlanSuspend();
    mico_thread_msleep(100);

    network_InitTypeDef_st wNetConfig;
    memset(&wNetConfig, 0x0, sizeof(network_InitTypeDef_st));
    strcpy((char *)wNetConfig.wifi_ssid, user_config->ap_name);
    strcpy((char *)wNetConfig.wifi_key, user_config->ap_key);
    wNetConfig.wifi_mode = Soft_AP;
    wNetConfig.dhcpMode = DHCP_Server;
    wNetConfig.wifi_retry_interval = 100;
    strcpy((char *)wNetConfig.local_ip_addr, ZZ_AP_LOCAL_IP);
    strcpy((char *)wNetConfig.net_mask, ZZ_AP_NET_MASK);
    strcpy((char *)wNetConfig.gateway_ip_addr, ZZ_AP_LOCAL_IP);
    strcpy((char *)wNetConfig.dnsServer_ip_addr, ZZ_AP_DNS_SERVER);
    err = micoWlanStart(&wNetConfig);

    if (err == kNoErr)
    {
        wifi_log("ApInit ssid[%s] key[******]", wNetConfig.wifi_ssid);
    }
    else
    {
        wifi_config_ap_active = false;
        wifi_log("ApInit failed[%d]", err);
    }
}

