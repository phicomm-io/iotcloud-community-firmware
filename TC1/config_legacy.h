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


static inline bool tc1_migrate_v10(void *data,uint32_t size)
{
    user_config_t *cfg = data;
    if (size != sizeof(user_config_v10_t) || cfg->version != 10) return false;
    memset((char *)data + size, 0, sizeof(*cfg) - size);
    memcpy(cfg->mqtt_user, cfg->legacy_mqtt_user, sizeof(cfg->legacy_mqtt_user));
    memcpy(cfg->mqtt_password, cfg->legacy_mqtt_password, sizeof(cfg->legacy_mqtt_password));
    cfg->version = USER_CONFIG_VERSION; cfg->mqtt_topic_mode = MQTT_TOPIC_LEGACY;
    return true;
}
