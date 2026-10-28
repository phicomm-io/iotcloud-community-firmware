#include "user_wifi.h"
#include "status_guard.h"

#include "main.h"
#include "../common/typed_wire.h"
#include "mico_socket.h"
#include "user_gpio.h"
#include "http_server/web_log.h"
#include "mqtt_server/user_mqtt_client.h"
#include "ota_server/signed_adapter.h"

char wifi_status = WIFI_STATE_NOCONNECT;

mico_timer_t wifi_led_timer;
IpStatus ip_status = { 0, ZZ_AP_LOCAL_IP, ZZ_AP_LOCAL_IP, ZZ_AP_NET_MASK };
static volatile bool wifi_reconnect_thread_running = false;
static volatile bool wifi_config_ap_active = false;
static volatile bool wifi_saved_ssid_seen = false;
static volatile bool wifi_boot_scan_pending = false;
static volatile bool wifi_ever_connected = false;
static struct { char ssid[33], key[65]; volatile bool active, dhcp; } wifi_trial;
static const char *wifi_trial_status = "idle";
const char *WifiTrialStatus(void) { return wifi_trial_status; }

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
    TC1_STATUS_WRITE(strcpy(ip_status.ip,pnet->ip);
    strcpy(ip_status.gateway,pnet->gate);strcpy(ip_status.mask,pnet->mask);
    wifi_status=WIFI_STATE_CONNECTED);

    if (wifi_trial.active && pnet->ip[0] && strcmp(pnet->ip,"0.0.0.0")) wifi_trial.dhcp = true;
    wifi_log("got IP:%s", pnet->ip);
    TC1_STATUS_WRITE(wifi_status = WIFI_STATE_CONNECTED);
    wifi_ever_connected = true;
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
        /* Link notifications must not persist unverified credentials. */

        OSStatus status = micoWlanSuspendSoftAP(); //鍏抽棴AP
        if (status != kNoErr)
        {
            wifi_log("close ap error[%d]", status);
        }

        TC1_STATUS_WRITE(ip_status.mode = 1);
        wifi_config_ap_active = false;
        wifi_saved_ssid_seen = true;
        //wifi_status = WIFI_STATE_CONNECTED;
    }
    else if (status == NOTIFY_STATION_DOWN) //wifi鏂紑
    {
        //涓嶄繚瀛楴OTIFY_STATION_DOWN鍒癴lash锛岀‘淇濋噸鍚悗浠嶄細灏濊瘯杩炴帴WiFi
        //sys_config->micoSystemConfig.reserved = status;
        //mico_system_context_update(sys_config);

        TC1_STATUS_WRITE(wifi_status = WIFI_STATE_NOCONNECT);
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
        TC1_STATUS_WRITE(ip_status.mode = 0);
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
bool WifiScanMp(ph_writer *out) {
    mico_rtos_lock_mutex(&scan_lock);
    mico_time_t now;mico_time_get_time(&now);
    if(scan_busy && (mico_time_t)(now-scan_started)>=20000)scan_busy=false;
    phw_begin(out,NULL,false);phw_bool(out,"scanning",scan_busy);phw_begin(out,"networks",true);
    for(int i=0;i<scan_count;i++){
        phw_begin(out,NULL,false);phw_text(out,"ssid",scan_results[i].ssid);
        phw_int(out,"rssi",scan_results[i].rssi);phw_int(out,"security",scan_results[i].security);phw_end(out);
    }
    phw_end(out);phw_end(out);mico_rtos_unlock_mutex(&scan_lock);return !out->failed;
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
    if (wifi_status == WIFI_STATE_CONNECTED && user_config->mqtt_host[0] && user_config->mqtt_port > 0)
        UserMqttInit();
    UserLedRefresh();
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
        TC1_STATUS_WRITE(wifi_status = WIFI_STATE_CONNECTED);
        goto exit;
    }

    TC1_STATUS_WRITE(wifi_status = WIFI_STATE_CONNECTING);
    wifi_log("WiFi reconnect started");

    /* Restart Station once; micoWlanStart keeps retrying in the background. */
    if (!wifi_trial.active) {
        micoWlanSuspendStation();
        mico_thread_msleep(100);
        if (!wifi_trial.active) WifiStartStationFromSavedConfig();
    }

    while (1)
    {
        if (wifi_status == WIFI_STATE_CONNECTED) break;
        /* Temporary AP does not terminate Station retries. */

        if (!WifiHasSavedConfig())
        {
            wifi_log("WiFi reconnect: no saved SSID, stop retry");
            break;
        }

        mico_thread_sleep(WIFI_RECONNECT_INTERVAL_SECONDS);
        elapsed += WIFI_RECONNECT_INTERVAL_SECONDS;
        if (allow_boot_ap && !wifi_ever_connected && !wifi_config_ap_active &&
            elapsed >= WIFI_BOOT_SCAN_TIMEOUT_SECONDS)
        {
            wifi_log("Boot connection timed out; temporary AP, retaining Station configuration");
            ApInit(true);
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

static void WifiTrialWorker(mico_thread_arg_t arg)
{
    network_InitTypeDef_st net = {0};
    strcpy(net.wifi_ssid,wifi_trial.ssid);strcpy(net.wifi_key,wifi_trial.key);
    net.wifi_mode=Station;net.dhcpMode=DHCP_Client;net.wifi_retry_interval=1000;
    micoWlanSuspendStation();wifi_trial.dhcp=false;
    uint32_t since=mico_rtos_get_time();
    bool verified=false;
    if (micoWlanStart(&net)==kNoErr) {
        while ((uint32_t)(mico_rtos_get_time()-since)<30000u) {
            LinkStatusTypeDef link={0};IPStatusTypedef ip={0};
            if (wifi_trial.dhcp && micoWlanGetLinkStatus(&link)==kNoErr && link.is_connected &&
                !strncmp((const char *)link.ssid,wifi_trial.ssid,sizeof(link.ssid)) &&
                micoWlanGetIPStatus(&ip,Station)==kNoErr && ip.ip[0] && strcmp(ip.ip,"0.0.0.0")) {
                verified=true;break;
            }
            mico_thread_msleep(100);
        }
    }
    bool saved=false;
    if (verified) {
        __typeof__(sys_config->micoSystemConfig) old=sys_config->micoSystemConfig;
        snprintf(sys_config->micoSystemConfig.ssid,sizeof(sys_config->micoSystemConfig.ssid),"%s",wifi_trial.ssid);
        snprintf(sys_config->micoSystemConfig.user_key,sizeof(sys_config->micoSystemConfig.user_key),"%s",wifi_trial.key);
        sys_config->micoSystemConfig.user_keyLength=strlen(wifi_trial.key);
        /* Invalidate the old derived PSK/BSSID: app starts from the verified passphrase. */
        sys_config->micoSystemConfig.keyLength=0;
        memset(sys_config->micoSystemConfig.bssid,0,6);
        saved=mico_system_context_update(sys_config)==kNoErr;
        if(!saved)sys_config->micoSystemConfig=old;
    }
    TC1_STATUS_WRITE(wifi_trial_status=saved?"success":verified?"storage_failure":"timeout_rollback");
    if (!saved) {
        micoWlanSuspendStation();
        if(WifiHasSavedConfig())WifiStartStationFromSavedConfig();else ApInit(true);
    }
    memset(wifi_trial.key,0,sizeof(wifi_trial.key));wifi_trial.active=false;
    tc1_config_release();
    mico_rtos_delete_thread(NULL);
}

bool WifiConnect(char* wifi_ssid, char* wifi_key)
{
    size_t n=strlen(wifi_key);
    if(!wifi_ssid[0] || strlen(wifi_ssid)>32 || (n && (n<8 || n>63)) || !tc1_config_reserve())return false;
    strcpy(wifi_trial.ssid,wifi_ssid);strcpy(wifi_trial.key,wifi_key);
    wifi_trial.active=true;wifi_trial.dhcp=false;TC1_STATUS_WRITE(wifi_trial_status="pending");
    if(mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY,"wifi_trial",WifiTrialWorker,4096,0)!=kNoErr) {
        memset(&wifi_trial,0,sizeof(wifi_trial));TC1_STATUS_WRITE(wifi_trial_status="storage_failure");
        tc1_config_release();return false;
    }
    return true;
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
        TC1_STATUS_WRITE(wifi_status = WIFI_STATE_CONNECTED);
        wifi_config_ap_active = false;
        TC1_STATUS_WRITE(ip_status.mode = 1);
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
    tc1_status_lock();
    strncpy(user_config->ap_name, name, 32);
    strncpy(user_config->ap_key, key, 32);
    user_config->ap_name[31] = 0;
    user_config->ap_key[31] = 0;
    tc1_status_unlock();
    wifi_log("ApConfig ap_name[%s] ap_key[******]", user_config->ap_name);
    ApInit(false);
    mico_system_context_update(sys_config);
}

void ApInit(bool use_defaul)
{
    OSStatus err;

    if (use_defaul)
    {
        tc1_status_lock();
        sprintf(user_config->ap_name, ZZ_AP_NAME, str_mac + 6);
        for (char *p = user_config->ap_name; *p; ++p)
            if (*p >= 'a' && *p <= 'f') *p -= 'a' - 'A';
        sprintf(user_config->ap_key, "%s", ZZ_AP_KEY);
        tc1_status_unlock();
        wifi_log("ApInit use_defaul[true] key[]");
    }

    wifi_config_ap_active = true;
    TC1_STATUS_WRITE(wifi_status = WIFI_STATE_NOCONNECT);

    /* SoftAP coexists with Station retries; never erase saved credentials. */

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

