#include "../common/service.h"

#include "../common/backup.h"
static bool config_recovery, config_commit_uncertain;
bool device_config_recovery(void) { return config_recovery || config_commit_uncertain; }
void device_config_recovery_set(bool recovery) { config_recovery=recovery || config_commit_uncertain; }
void app_config_commit_indeterminate(void) { config_commit_uncertain=config_recovery=true; }
bool app_config_persist_allowed(void) { return !config_recovery && !config_commit_uncertain; }
#include "ota_server/signed_adapter.h"
#include "main.h"
#include "status_guard.h"
#include "config_legacy.h"
#include <stddef.h>

uint32_t app_config_legacy_size(void) { return sizeof(user_config_v12_t); }
uint32_t app_config_legacy_next_size(uint32_t size) { return size==sizeof(user_config_v12_t)?offsetof(user_config_t,bemfa):size==offsetof(user_config_t,bemfa)?sizeof(user_config_v11_t):size==sizeof(user_config_v11_t)?sizeof(user_config_v10_t):0; }
bool app_config_migrate_legacy(void *data, uint32_t size)
{
    if(size==sizeof(user_config_v12_t) && ((user_config_t *)data)->version==USER_CONFIG_VERSION) {
        ph_auth_store_empty(&((user_config_t *)data)->local_auth);return true;
    }
    if(size==offsetof(user_config_t,bemfa) && ((user_config_t *)data)->version==USER_CONFIG_VERSION) {
        memset(&((user_config_t *)data)->bemfa,0,sizeof(bemfa_config_t));ph_auth_store_empty(&((user_config_t *)data)->local_auth);return true;
    }
    bool ok=tc1_migrate_v11(data,size) || tc1_migrate_v10(data,size);
    if(ok)ph_auth_store_empty(&((user_config_t *)data)->local_auth);return ok;
}

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"
#include "unistd.h"
#include "TimeUtils.h"
#include "mico_system.h"

#include "user_gpio.h"
#include "user_wifi.h"
#include "time_server/user_rtc.h"
#include "user_power.h"
#include "mqtt_server/user_mqtt_client.h"
#include "http_server/app_httpd.h"
#include "timed_task/timed_task.h"

char rtc_init = 0; //sntp鏍℃椂鎴愬姛鏍囧織浣?
uint32_t total_time = 0;
char str_mac[16] = {0};
int last_check_day = 0;
int childLockEnabled = 0;

system_config_t *sys_config;
user_config_t *user_config;

mico_gpio_t Relay[Relay_NUM] = {Relay_0, Relay_1, Relay_2, Relay_3, Relay_4, Relay_5};

#define DEFAULT_SOCKET_NAME_FORMAT "\xE6\x8F\x92\xE5\xBA\xA7-%d"
#define CORRUPTED_DEFAULT_SOCKET_NAME_FORMAT "\xE9\x8E\xBB\xE6\x8E\x91\xE9\xAA\x87-%d"

static bool migrateCorruptedDefaultSocketNames(void) {
    bool updated = false;
    char corrupted_name[SOCKET_NAME_LENGTH];

    for (int i = 0; i < SOCKET_NUM; i++) {
        snprintf(corrupted_name, sizeof(corrupted_name),
                 CORRUPTED_DEFAULT_SOCKET_NAME_FORMAT, i + 1);
        if (strcmp(user_config->socket_names[i], corrupted_name) == 0) {
            snprintf(user_config->socket_names[i], SOCKET_NAME_LENGTH,
                     DEFAULT_SOCKET_NAME_FORMAT, i + 1);
            updated = true;
        }
    }

    return updated;
}

static bool migratePowerCoefficient(void) {
    if (user_config->power_coefficient_x100 < POWER_COEFFICIENT_MIN_X100 ||
        user_config->power_coefficient_x100 > POWER_COEFFICIENT_MAX_X100) {
        user_config->power_coefficient_x100 = POWER_COEFFICIENT_BL0937_X100;
        return true;
    }
    return false;
}

#include "local_auth.inc"

/* MICO system callback: Restore default configuration provided by application */
void appRestoreDefault_callback(void *const user_config_data, uint32_t size) {
    UNUSED_PARAMETER(size);

    mico_system_context_get()->micoSystemConfig.name[0] = 1; //鍦ㄤ笅娆￠噸鍚椂浣跨敤榛樿鍚嶇О
    mico_system_context_get()->micoSystemConfig.name[1] = 0;

    user_config_t *userConfigDefault = user_config_data;
    memset(userConfigDefault,0,sizeof(*userConfigDefault));
    ph_auth_store_empty(&userConfigDefault->local_auth);
    automation_defaults(&userConfigDefault->automation);
    userConfigDefault->user[0] = 0;
    userConfigDefault->mqtt_ip[0] = 0;
    userConfigDefault->mqtt_port = 0;
    userConfigDefault->mqtt_user[0] = 0;
    userConfigDefault->mqtt_password[0] = 0;
    userConfigDefault->mqtt_topic_mode = MQTT_TOPIC_TMCP;
    userConfigDefault->web_password[0] = 0;
    userConfigDefault->task_top = NULL;
    userConfigDefault->task_count = 0;
    userConfigDefault->mqtt_report_freq = 2;
    userConfigDefault->p_count_2_days_ago = 0;
    userConfigDefault->p_count_1_day_ago = 0;
    userConfigDefault->power_led_enabled = 1;
    userConfigDefault->power_coefficient_x100 = POWER_COEFFICIENT_BL0937_X100;
    userConfigDefault->version = USER_CONFIG_VERSION;
    set_key_map(userConfigDefault->user,1, SWITCH_ALL_SOCKETS, NO_FUNCTION);
    for (int i = 2; i < 32; i++) {
        int longFunc = NO_FUNCTION;
        //鍑哄巶璁剧疆锛岄暱鎸?绉掑紑鍚厤缃戞ā寮忥紝闀挎寜10绉掓仮澶嶅嚭鍘傝缃?
        if (i >=5 && i< 10) {
            longFunc = CONFIG_WIFI;
        } else if (i >= 10 && i< 15) {
            longFunc = RESET_SYSTEM;
        }
        set_key_map(userConfigDefault->user,i, NO_FUNCTION, longFunc);
    }

    for (int i = 0; i < SOCKET_NUM; i++) {
        userConfigDefault->socket_status[i] = 1;
        snprintf(userConfigDefault->socket_names[i], SOCKET_NAME_LENGTH,
                 DEFAULT_SOCKET_NAME_FORMAT, i + 1);
    }
    for (int i = 0; i < MAX_TASK_NUM; i++) {
        userConfigDefault->timed_tasks[i].on_use = false;
    }
    mico_system_context_update(sys_config);
}

void recordDailyPCount(void) {
    if(rtc_init!=1)return;
    mico_utc_time_t utc;mico_time_get_utc_time(&utc);
    int day=(utc+28800)/86400;
    if(last_check_day && day>last_check_day){
        user_config->p_count_2_days_ago=user_config->p_count_1_day_ago;
        user_config->p_count_1_day_ago=p_count;
        mico_system_context_update(sys_config);
    }
    last_check_day=day;
}

void schedule_p_count_task(mico_thread_arg_t arg) {
    mico_thread_sleep(20);tc1_log("WARNGIN: p_count timer thread created!");
    while (1) {
        recordDailyPCount();
        mico_thread_sleep(60);
    }
}

void reportMqttPowerInfoThread() {
    while (1) {
        UserMqttHassPower();
        int freq = user_config->mqtt_report_freq;

        if (freq == 0) {
            freq = 2;
        }

        mico_thread_msleep(1000 * freq);
    }
}

int application_start(void) {
    if(!tc1_status_guard_init())return kNoMemoryErr;
    service_init();
    WebLogInit();
    if(!tc1_ota_init())return kNoMemoryErr;
    int i;tc1_log("start version[%s]", VERSION);

    //char main_num=0;
    OSStatus err = kNoErr;

    // Create mico system context and read application's config data from flash
    const mico_logic_partition_t *parameter=MicoFlashGetInfo(MICO_PARTITION_PARAMETER_1);
    const mico_logic_partition_t *backup=MicoFlashGetInfo(MICO_PARTITION_PARAMETER_2);
    require_action(parameter && backup && sizeof(system_config_t)+sizeof(user_config_t)+2 <= parameter->partition_length && sizeof(system_config_t)+sizeof(user_config_t)+2 <= backup->partition_length,exit,err=kNoMemoryErr);
    sys_config = mico_system_context_init(sizeof(user_config_t));
    require_action(sys_config,exit,err=kNoMemoryErr);
    user_config = ((system_context_t *) sys_config)->user_config_data;
    require_action(user_config, exit, err = kNoMemoryErr);

    if(user_config->version!=USER_CONFIG_VERSION) {
        config_recovery=true;memset(user_config,0,sizeof(*user_config));
        appRestoreDefault_callback(user_config,sizeof(*user_config));
    }
    if (user_config->automation.magic != AUTOMATION_MAGIC) {
        automation_defaults(&user_config->automation);
        for(unsigned i=0,n=0;i<MAX_TASK_NUM;i++) if(user_config->timed_tasks[i].on_use) {
            struct TimedTask *old=&user_config->timed_tasks[i];automation_task_t *t=&user_config->automation.tasks[n++];
            t->id=user_config->automation.next_id++;t->action.target=255;t->action.value=old->on!=0;
            if(old->operation<=6){t->action.kind=AUTO_OUTLET;if(old->operation)t->action.target=old->operation-1;}
            else if(old->operation==7)t->action.kind=AUTO_INDICATOR;
            else if(old->operation==8)t->action.kind=AUTO_REBOOT;
            else if(old->operation==11)t->action.kind=AUTO_CHILD_LOCK;
            t->enabled=t->action.kind!=AUTO_NONE;t->state=t->enabled?AUTO_WAITING:AUTO_MIGRATED_DISABLED;
            if(old->weekday){t->days=old->weekday==8?127:1u<<(old->weekday-1);t->minute=((old->prs_time+28800)%86400)/60;}
            else{t->once=1;t->at=old->prs_time;}
        }
        for(unsigned i=1;i<=30;i++) for(unsigned hold=0;hold<2;hold++) {
            if(hold && i>4)continue;int op=hold?get_long_func(user_config->user[i]):get_short_func(user_config->user[i]);
            automation_action_t *a=&user_config->automation.buttons[0][hold?i+29:i-1];memset(a,0,sizeof(*a));a->value=2;a->target=255;
            if(op>=0 && op<=6){a->kind=AUTO_OUTLET;if(op)a->target=op-1;}else if(op==8){a->kind=AUTO_REBOOT;a->value=0;}
        }
        memset(user_config->timed_tasks,0,sizeof(user_config->timed_tasks));user_config->task_count=0;user_config->task_top=NULL;
        err=mico_system_context_update(sys_config);require_noerr(err,exit);
    }
    err = mico_system_init(sys_config);
    require_noerr(err, exit);

    uint8_t mac[8];
    mico_wlan_get_mac_address(mac);
    sprintf(str_mac, "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);tc1_log("str_mac[%s]", str_mac);

    bool open_ap = false;
    MicoGpioInitialize((mico_gpio_t) Button, INPUT_PULL_UP);
    if (!MicoGpioInputGet(Button)) {   //寮€鏈烘椂鎸夐挳鐘舵€?
        tc1_log("press ap_init");
        ApInit(true);
        open_ap = true;
    }

    MicoGpioInitialize((mico_gpio_t) Led, OUTPUT_PUSH_PULL);
    for (i = 0; i < Relay_NUM; i++) {
        MicoGpioInitialize(Relay[i], OUTPUT_PUSH_PULL);
        UserRelaySet(i, user_config->socket_status[i]);
    }
    MicoSysLed(0);

    childLockEnabled = (int) user_config->user[0];
    if (user_config->version != USER_CONFIG_VERSION) { tc1_log("WARNGIN: user params restored!");
        err = mico_system_context_restore(sys_config);
        require_noerr(err, exit);
    }

    bool mqtt_extension=user_config->bemfa.magic!=BEMFA_MAGIC || user_config->bemfa.version!=1;
    if(mqtt_extension)bemfa_defaults(&user_config->bemfa,"tc1",str_mac);
    bool config_updated = migrateCorruptedDefaultSocketNames();
    config_updated |= mqtt_extension;
    config_updated |= migratePowerCoefficient();
    if (config_updated) {
        mico_system_context_update(sys_config);
    }

    if (sys_config->micoSystemConfig.name[0] == 1) {
        sprintf(sys_config->micoSystemConfig.name, ZTC1_NAME, str_mac + 8);
    }

    tc1_log("device name:%s",
            sys_config->micoSystemConfig.name);tc1_log(
            "mqtt_ip:%s", user_config->mqtt_host);tc1_log("mqtt_port:%d",
                                                        user_config->mqtt_port);tc1_log(
            "mqtt_user:%s", user_config->mqtt_user);
    //tc1_log("mqtt_password:%s",user_config->mqtt_password);
    tc1_log("version:%d", user_config->version);

    require_action(device_local_auth_init(),exit,err=kNoMemoryErr);
    TasksInit();
    WifiInit();
    if (!open_ap) {
        WifiStartOnBoot();
    }
    KeyInit();
    err = UserRtcInit();
    require_noerr(err, exit);
    PowerInit();
    AppHttpdStart(); // start http server thread

    UserLedSet(user_config->power_led_enabled);

    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "p_count",
                                  (mico_thread_function_t) schedule_p_count_task,
                                  0x800, 0);
    require_noerr_string(err, exit, "ERROR: Unable to start the p_count thread.");

    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "mqtt_power_report",
                                  (mico_thread_function_t) reportMqttPowerInfoThread,
                                  0x1000, 0);
    require_noerr_string(err, exit, "ERROR: Unable to start the mqtt_power_report thread.");


    while (1) {
        automation_poll(rtc_init == 1);
        mico_thread_msleep(1000);
    }

    exit:tc1_log("application_start ERROR!");
    return 0;
}


#include "../common/device_status.h"
bool tc1_status_capture(device_status_t *snapshot, device_status_full_t *full)
{
    memset(snapshot,0,sizeof(*snapshot));if(full)memset(full,0,sizeof(*full));
#ifdef PHICOMM_BEMFA
    bemfa_status_capture(&snapshot->network);
#endif
    if(!tc1_status_begin())return false;
    snapshot->model=DEVICE_TC1;
    snapshot->captured=STATUS_IDENTITY|STATUS_NETWORK|STATUS_CONTROL|STATUS_OTA;
    snapshot->identity.version=VERSION;
    device_status_text(snapshot->identity.id,sizeof(snapshot->identity.id),str_mac,sizeof(str_mac));
    device_status_text(snapshot->identity.name,sizeof(snapshot->identity.name),sys_config->micoSystemConfig.name,sizeof(sys_config->micoSystemConfig.name));
    memcpy(snapshot->control.tc1.states,user_config->socket_status,sizeof(snapshot->control.tc1.states));
    snapshot->control.tc1.child_lock=childLockEnabled!=0;
    snapshot->control.tc1.led_enabled=user_config->power_led_enabled!=0;
    snapshot->network.wifi_connected=wifi_status==WIFI_STATE_CONNECTED;
    snapshot->network.ap_active=ip_status.mode==0;
    snapshot->network.mqtt_status=tc1_mqtt_status;
    snapshot->network.mqtt_connected=tc1_mqtt_status==MQTT_READY;
    snapshot->network.mqtt_cleanup_warning=tc1_mqtt_cleanup_warning;
    strncpy(snapshot->network.wifi_trial_status,WifiTrialStatus(),sizeof(snapshot->network.wifi_trial_status)-1);
    if(full){
        snapshot->captured|=STATUS_DIAGNOSTIC;
        device_status_text(full->network.ssid,sizeof(full->network.ssid),sys_config->micoSystemConfig.ssid,sizeof(sys_config->micoSystemConfig.ssid));
        device_status_text(full->network.ip,sizeof(full->network.ip),ip_status.ip,sizeof(ip_status.ip));
        device_status_text(full->model.tc1.mask,sizeof(full->model.tc1.mask),ip_status.mask,sizeof(ip_status.mask));
        device_status_text(full->model.tc1.gateway,sizeof(full->model.tc1.gateway),ip_status.gateway,sizeof(ip_status.gateway));
        device_status_text(full->network.ap_name,sizeof(full->network.ap_name),user_config->ap_name,sizeof(user_config->ap_name));
        device_status_text(full->network.mqtt_host,sizeof(full->network.mqtt_host),user_config->mqtt_host,sizeof(user_config->mqtt_host));
        device_status_text(full->network.mqtt_user,sizeof(full->network.mqtt_user),user_config->mqtt_user,sizeof(user_config->mqtt_user));
        full->network.mqtt_port=user_config->mqtt_port;
        for(unsigned i=0;i<6;i++)device_status_text(full->model.tc1.socket_names[i],sizeof(full->model.tc1.socket_names[i]),user_config->socket_names[i],sizeof(user_config->socket_names[i]));
        full->model.tc1.modern=user_config->mqtt_topic_mode!=0;
        full->model.tc1.time_synced=rtc_init==1;
        full->model.tc1.report_seconds=user_config->mqtt_report_freq;
        full->model.tc1.power_coefficient=user_config->power_coefficient_x100;
        full->network.wifi_password_set=sys_config->micoSystemConfig.user_key[0]!=0;
        full->network.mqtt_password_set=user_config->mqtt_password[0]!=0;
        full->network.web_password_set=user_config->web_password[0]!=0;
    }
    tc1_status_unlock();
    tc1_ota_snapshot(snapshot->ota.state,snapshot->ota.error,&snapshot->ota.progress);
    snapshot->now_ms=mico_rtos_get_time();
    if(full){
        full->model.tc1.utc_time=(int32_t)time(NULL);
        const micoMemInfo_t *memory=MicoGetMemoryInfo();
        if(memory){full->runtime.memory_valid=true;full->runtime.free_memory=memory->free_memory;}
    }
    return true;
}

void bemfa_config_get(bemfa_config_t *out) {
    tc1_status_lock();*out=user_config->bemfa;tc1_status_unlock();
}
bool bemfa_primary_enabled(void) {tc1_status_lock();bool enabled=user_config->bemfa.mqtt_enabled;tc1_status_unlock();return enabled;}
const char *bemfa_device_id(void) {return str_mac;}
bool bemfa_network_ready(void) {
    LinkStatusTypeDef link={0};micoWlanGetLinkStatus(&link);
    return link.is_connected==1 && !tc1_ota_active();
}

#include "backup.inc"
