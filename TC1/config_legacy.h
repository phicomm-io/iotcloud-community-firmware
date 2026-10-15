#pragma once
typedef struct
{
    char version;
    char mqtt_ip[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_names[SOCKET_NUM][SOCKET_NAME_LENGTH];
    int mqtt_port;
    int mqtt_report_freq;
    char mqtt_user[SETTING_MQTT_STRING_LENGTH_MAX];
    char mqtt_password[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_status[SOCKET_NUM]; //记录当前开关
    char user[maxNameLen];
    WiFiEvent last_wifi_status;
    char ap_name[32];
    char ap_key[32];
    int task_count;
    int p_count_2_days_ago;
    int p_count_1_day_ago;
    int power_led_enabled;
    pTimedTask task_top;
    struct TimedTask timed_tasks[MAX_TASK_NUM];
    char web_password[WEB_PASSWORD_LENGTH_MAX + 1];
    uint16_t power_coefficient_x100;
} user_config_v10_t;


typedef struct
{
    char version;
    char mqtt_ip[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_names[SOCKET_NUM][SOCKET_NAME_LENGTH];
    int mqtt_port;
    int mqtt_report_freq;
    char legacy_mqtt_user[SETTING_MQTT_STRING_LENGTH_MAX];
    char legacy_mqtt_password[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_status[SOCKET_NUM]; //记录当前开关
    char user[maxNameLen];
    WiFiEvent last_wifi_status;
    char ap_name[32];
    char ap_key[32];
    int task_count;
    int p_count_2_days_ago;
    int p_count_1_day_ago;
    int power_led_enabled;
    pTimedTask task_top;
    struct TimedTask timed_tasks[MAX_TASK_NUM];
    char web_password[WEB_PASSWORD_LENGTH_MAX + 1];
    uint16_t power_coefficient_x100;
    char mqtt_user[MQTT_USER_MAX + 1];
    char mqtt_password[MQTT_PASSWORD_MAX + 1];
    uint8_t mqtt_topic_mode;
} user_config_v11_t;

static inline bool tc1_migrate_v11(void *data,uint32_t size) {
 user_config_t *cfg=data;if(size!=sizeof(user_config_v11_t) || cfg->version!=11)return false;memset((char *)data+size,0,sizeof(*cfg)-size);strcpy(cfg->mqtt_host,cfg->mqtt_ip);cfg->version=USER_CONFIG_VERSION;return true;
}

static inline bool tc1_migrate_v10(void *data,uint32_t size)
{
    user_config_t *cfg = data;
    if (size != sizeof(user_config_v10_t) || cfg->version != 10) return false;
    memset((char *)data + size, 0, sizeof(*cfg) - size);
    memcpy(cfg->mqtt_user, cfg->legacy_mqtt_user, sizeof(cfg->legacy_mqtt_user));
    memcpy(cfg->mqtt_password, cfg->legacy_mqtt_password, sizeof(cfg->legacy_mqtt_password));
    strcpy(cfg->mqtt_host,cfg->mqtt_ip);
    cfg->version = USER_CONFIG_VERSION;
    /* A legacy config that never had a broker keeps TMCP once it is set up. */
    cfg->mqtt_topic_mode = cfg->mqtt_host[0] ? MQTT_TOPIC_LEGACY : MQTT_TOPIC_TMCP;
    return true;
}
